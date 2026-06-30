#!/usr/bin/env node
import fs from 'node:fs';
import readline from 'node:readline';

function argsMap(argv) {
  const args = new Map();
  for (let i = 2; i < argv.length; ++i) {
    const key = argv[i];
    if (!key.startsWith('--')) continue;
    if (i + 1 < argv.length && !argv[i + 1].startsWith('--')) args.set(key.slice(2), argv[++i]);
    else args.set(key.slice(2), 'true');
  }
  return args;
}

function required(args, key) {
  const value = args.get(key);
  if (!value) throw new Error(`Missing --${key}`);
  return value;
}

function jsonString(line, key) {
  const needle = `"${key}"`;
  const keyPos = line.indexOf(needle);
  if (keyPos < 0) return '';
  const colon = line.indexOf(':', keyPos + needle.length);
  if (colon < 0) return '';
  let pos = colon + 1;
  while (pos < line.length && /\s/.test(line[pos])) ++pos;
  if (line[pos] !== '"') return '';
  ++pos;
  let out = '';
  while (pos < line.length) {
    const ch = line[pos++];
    if (ch === '"') return out;
    if (ch !== '\\') {
      out += ch;
      continue;
    }
    if (pos >= line.length) return out;
    const esc = line[pos++];
    if (esc === '"' || esc === '\\' || esc === '/') out += esc;
    else if (esc === 'b') out += '\b';
    else if (esc === 'f') out += '\f';
    else if (esc === 'n') out += '\n';
    else if (esc === 'r') out += '\r';
    else if (esc === 't') out += '\t';
    else if (esc === 'u' && pos + 4 <= line.length) {
      out += String.fromCharCode(parseInt(line.slice(pos, pos + 4), 16));
      pos += 4;
    } else {
      out += esc;
    }
  }
  return out;
}

function loadQueries(filePath) {
  const map = new Map();
  for (const line of fs.readFileSync(filePath, 'utf8').split(/\r?\n/)) {
    if (!line) continue;
    const id = jsonString(line, '_id');
    if (id) map.set(id, jsonString(line, 'text'));
  }
  return map;
}

async function loadDocs(filePath, wanted) {
  const map = new Map();
  const input = fs.createReadStream(filePath, { encoding: 'utf8' });
  const reader = readline.createInterface({ input, crlfDelay: Infinity });
  for await (const line of reader) {
    if (!line) continue;
    const id = jsonString(line, '_id');
    if (!wanted.has(id)) continue;
    map.set(id, {
      title: jsonString(line, 'title'),
      text: jsonString(line, 'text'),
    });
    if (map.size >= wanted.size) {
      reader.close();
      break;
    }
  }
  return map;
}

function loadPerQuery(filePath, count) {
  const lines = fs.readFileSync(filePath, 'utf8').split(/\r?\n/).filter(Boolean);
  const header = lines.shift().split('\t');
  const rows = lines.map(line => {
    const values = line.split('\t');
    const row = {};
    for (let i = 0; i < header.length; ++i) row[header[i]] = values[i] ?? '';
    row.delta_right_minus_left = Number(row.delta_right_minus_left ?? 0);
    return row;
  });
  return rows.sort((a, b) => a.delta_right_minus_left - b.delta_right_minus_left).slice(0, count);
}

function queryTokens(text) {
  return text.toLowerCase().match(/[\p{L}\p{N}]+/gu) ?? [];
}

function queryBigrams(tokens) {
  const out = [];
  for (let i = 0; i + 1 < tokens.length; ++i) out.push(`${tokens[i]}_${tokens[i + 1]}`);
  return out;
}

function docContainsAll(docText, tokens) {
  const text = docText.toLowerCase();
  return tokens.filter(token => text.includes(token));
}

function docContainsBigrams(docText, bigrams) {
  const compact = docText.toLowerCase().replace(/[^\p{L}\p{N}]+/gu, ' ').trim();
  return bigrams.filter(bigram => compact.includes(bigram.replace('_', ' ')));
}

async function main() {
  const args = argsMap(process.argv);
  const dataDir = required(args, 'data');
  const perQuery = required(args, 'per-query');
  const outPath = required(args, 'out');
  const count = Number(args.get('count') ?? '30');
  const rows = loadPerQuery(perQuery, count);
  const queries = loadQueries(`${dataDir}/queries.jsonl`);
  const wantedDocs = new Set();
  for (const row of rows) {
    for (const field of ['lucene_only_docs', 'moon_only_docs']) {
      for (const docid of (row[field] ?? '').split(';')) if (docid) wantedDocs.add(docid);
    }
  }
  const docs = await loadDocs(`${dataDir}/corpus.jsonl`, wantedDocs);

  const out = [];
  for (const row of rows) {
    out.push(`# qid=${row.qid} delta=${row.delta_right_minus_left} lucene_only=${row.lucene_only} moon_only=${row.moon_only}`);
    const query = queries.get(row.qid) ?? '';
    const tokens = queryTokens(query);
    const bigrams = queryBigrams(tokens);
    out.push(`query: ${query}`);
    out.push(`tokens: ${tokens.join(', ')}`);
    out.push(`bigrams: ${bigrams.join(', ')}`);
    for (const docid of (row.lucene_only_docs ?? '').split(';').filter(Boolean)) {
      const doc = docs.get(docid) ?? { title: '', text: '' };
      const fullText = `${doc.title} ${doc.text}`;
      out.push(`lucene_only_doc ${docid}`);
      out.push(`  matched_tokens: ${docContainsAll(fullText, tokens).join(', ')}`);
      out.push(`  matched_bigrams: ${docContainsBigrams(fullText, bigrams).join(', ')}`);
      out.push(`  title: ${doc.title}`);
      out.push(`  text: ${doc.text.slice(0, 800).replace(/\s+/g, ' ')}`);
    }
    for (const docid of (row.moon_only_docs ?? '').split(';').filter(Boolean)) {
      const doc = docs.get(docid) ?? { title: '', text: '' };
      const fullText = `${doc.title} ${doc.text}`;
      out.push(`moon_only_doc ${docid}`);
      out.push(`  matched_tokens: ${docContainsAll(fullText, tokens).join(', ')}`);
      out.push(`  matched_bigrams: ${docContainsBigrams(fullText, bigrams).join(', ')}`);
      out.push(`  title: ${doc.title}`);
      out.push(`  text: ${doc.text.slice(0, 800).replace(/\s+/g, ' ')}`);
    }
    out.push('');
  }
  fs.writeFileSync(outPath, out.join('\n'), 'utf8');
  console.log(`wrote ${outPath}`);
}

await main();
