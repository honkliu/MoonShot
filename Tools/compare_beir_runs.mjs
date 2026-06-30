#!/usr/bin/env node
import fs from 'node:fs';
import path from 'node:path';

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

function loadQrels(filePath) {
  const qrels = new Map();
  const lines = fs.readFileSync(filePath, 'utf8').split(/\r?\n/);
  for (const line of lines) {
    if (!line) continue;
    const columns = line.split('\t');
    if (columns.length < 3 || columns[0] === 'query-id') continue;
    const score = Number(columns[2]);
    if (!Number.isFinite(score) || score <= 0) continue;
    if (!qrels.has(columns[0])) qrels.set(columns[0], new Set());
    qrels.get(columns[0]).add(columns[1]);
  }
  return qrels;
}

function loadRun(filePath, maxRank, qrels) {
  const run = new Map();
  const lines = fs.readFileSync(filePath, 'utf8').split(/\r?\n/);
  for (const line of lines) {
    if (!line) continue;
    const parts = line.trim().split(/\s+/);
    if (parts.length < 6) continue;
    const [qid, , docid, rankText, scoreText] = parts;
    const rank = Number(rankText);
    const score = Number(scoreText);
    if (!Number.isFinite(rank) || !Number.isFinite(score) || rank > maxRank) continue;
    if (!qrels.get(qid)?.has(docid)) continue;
    if (!run.has(qid)) run.set(qid, new Map());
    const docs = run.get(qid);
    if (!docs.has(docid)) docs.set(docid, { rank, score });
  }
  return run;
}

function sortedDocs(docSet, runDocs) {
  return [...docSet].sort((a, b) => (runDocs?.get(a)?.rank ?? 1e9) - (runDocs?.get(b)?.rank ?? 1e9));
}

function intersection(a, b) {
  const out = new Set();
  for (const value of a) if (b.has(value)) out.add(value);
  return out;
}

function difference(a, b) {
  const out = new Set();
  for (const value of a) if (!b.has(value)) out.add(value);
  return out;
}

function main() {
  const args = argsMap(process.argv);
  const qrelsPath = required(args, 'qrels');
  const leftPath = required(args, 'left');
  const rightPath = required(args, 'right');
  const leftName = args.get('left-name') ?? 'left';
  const rightName = args.get('right-name') ?? 'right';
  const k = Number(args.get('k') ?? '1000');
  const top = Number(args.get('top') ?? '30');
  const outPrefix = args.get('out-prefix') ?? '';

  const qrels = loadQrels(qrelsPath);
  const left = loadRun(leftPath, k, qrels);
  const right = loadRun(rightPath, k, qrels);
  const qids = [...qrels.keys()].filter(qid => left.has(qid) || right.has(qid)).sort();

  let totalRelevant = 0;
  let totalCommon = 0;
  let totalLeftOnly = 0;
  let totalRightOnly = 0;
  let totalLeftHits = 0;
  let totalRightHits = 0;
  const rows = [];

  for (const qid of qids) {
    const relevant = qrels.get(qid);
    const leftHits = intersection(new Set(left.get(qid)?.keys() ?? []), relevant);
    const rightHits = intersection(new Set(right.get(qid)?.keys() ?? []), relevant);
    const common = intersection(leftHits, rightHits);
    const leftOnly = difference(leftHits, rightHits);
    const rightOnly = difference(rightHits, leftHits);
    const delta = rightHits.size - leftHits.size;

    totalRelevant += relevant.size;
    totalCommon += common.size;
    totalLeftOnly += leftOnly.size;
    totalRightOnly += rightOnly.size;
    totalLeftHits += leftHits.size;
    totalRightHits += rightHits.size;

    rows.push({
      qid,
      relevant: relevant.size,
      [`${leftName}_hits`]: leftHits.size,
      [`${rightName}_hits`]: rightHits.size,
      common: common.size,
      [`${leftName}_only`]: leftOnly.size,
      [`${rightName}_only`]: rightOnly.size,
      delta_right_minus_left: delta,
      [`${leftName}_only_docs`]: sortedDocs(leftOnly, left.get(qid)).slice(0, 10).join(';'),
      [`${rightName}_only_docs`]: sortedDocs(rightOnly, right.get(qid)).slice(0, 10).join(';'),
    });
  }

  console.log(`queries=${qids.length} k=${k} relevant=${totalRelevant}`);
  console.log(`${leftName}_hits=${totalLeftHits} ${rightName}_hits=${totalRightHits} delta=${totalRightHits - totalLeftHits}`);
  console.log(`common=${totalCommon} ${leftName}_only=${totalLeftOnly} ${rightName}_only=${totalRightOnly}`);
  if (totalRelevant) {
    console.log(`recall_${leftName}=${(totalLeftHits / totalRelevant).toFixed(6)} recall_${rightName}=${(totalRightHits / totalRelevant).toFixed(6)}`);
  }

  const worst = [...rows].sort((a, b) => a.delta_right_minus_left - b.delta_right_minus_left).slice(0, top);
  const best = [...rows].sort((a, b) => b.delta_right_minus_left - a.delta_right_minus_left).slice(0, top);
  console.log('\nWorst right-minus-left queries:');
  for (const row of worst) console.log(JSON.stringify(row));
  console.log('\nBest right-minus-left queries:');
  for (const row of best) console.log(JSON.stringify(row));

  if (outPrefix && rows.length) {
    const outputPath = `${outPrefix}.per_query.tsv`;
    const fields = Object.keys(rows[0]);
    const body = [fields.join('\t'), ...rows.map(row => fields.map(field => row[field] ?? '').join('\t'))].join('\n') + '\n';
    fs.mkdirSync(path.dirname(outputPath), { recursive: true });
    fs.writeFileSync(outputPath, body, 'utf8');
    console.log(`wrote ${outputPath}`);
  }
}

main();
