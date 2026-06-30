#!/usr/bin/env node
import fs from 'node:fs';

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
function required(args, key) { const v = args.get(key); if (!v) throw new Error(`Missing --${key}`); return v; }
function loadRun(path, weight) {
  const run = new Map();
  for (const line of fs.readFileSync(path, 'utf8').split(/\r?\n/)) {
    if (!line) continue;
    const p = line.trim().split(/\s+/);
    if (p.length < 6) continue;
    const [qid,,docid,rankText,scoreText] = p;
    const rank = Number(rankText), score = Number(scoreText);
    if (!run.has(qid)) run.set(qid, new Map());
    const docs = run.get(qid);
    const normalized = weight * (score + 1.0) + weight * (1.0 / Math.max(1, rank));
    docs.set(docid, Math.max(docs.get(docid) ?? -Infinity, normalized));
  }
  return run;
}
function loadQrels(path) {
  const qrels = new Map();
  for (const line of fs.readFileSync(path, 'utf8').split(/\r?\n/)) {
    if (!line) continue;
    const c = line.split('\t');
    if (c.length < 3 || c[0] === 'query-id') continue;
    const score = Number(c[2]);
    if (!Number.isFinite(score) || score <= 0) continue;
    if (!qrels.has(c[0])) qrels.set(c[0], new Set());
    qrels.get(c[0]).add(c[1]);
  }
  return qrels;
}
function mergeRuns(left, right, k) {
  const merged = new Map();
  const qids = new Set([...left.keys(), ...right.keys()]);
  for (const qid of qids) {
    const scores = new Map();
    for (const [doc, score] of left.get(qid) ?? []) scores.set(doc, (scores.get(doc) ?? 0) + score);
    for (const [doc, score] of right.get(qid) ?? []) scores.set(doc, (scores.get(doc) ?? 0) + score);
    const ranked = [...scores.entries()].sort((a,b) => b[1] - a[1] || a[0].localeCompare(b[0])).slice(0, k);
    merged.set(qid, ranked);
  }
  return merged;
}
function evalRun(run, qrels, at) {
  let evaluated = 0, relevantTotal = 0;
  const hits = at.map(() => 0), macro = at.map(() => 0);
  for (const [qid, relevant] of qrels) {
    const ranked = run.get(qid);
    if (!ranked) continue;
    let count = 0, next = 0;
    const cumulative = at.map(() => 0);
    for (let i = 0; i < ranked.length; ++i) {
      if (relevant.has(ranked[i][0])) count++;
      while (next < at.length && i + 1 === at[next]) cumulative[next++] = count;
    }
    while (next < at.length) cumulative[next++] = count;
    evaluated++; relevantTotal += relevant.size;
    for (let i = 0; i < at.length; ++i) { hits[i] += cumulative[i]; macro[i] += cumulative[i] / relevant.size; }
  }
  return {evaluated, relevantTotal, hits, macro};
}
const args = argsMap(process.argv);
const left = loadRun(required(args, 'left'), Number(args.get('left-weight') ?? '1'));
const right = loadRun(required(args, 'right'), Number(args.get('right-weight') ?? '1'));
const k = Number(args.get('k') ?? '1000');
const merged = mergeRuns(left, right, k);
const out = required(args, 'out');
let text = '';
for (const [qid, ranked] of merged) {
  ranked.forEach(([doc, score], i) => { text += `${qid} Q0 ${doc} ${i + 1} ${score.toFixed(9)} merged\n`; });
}
fs.writeFileSync(out, text, 'utf8');
if (args.has('qrels')) {
  const qrels = loadQrels(args.get('qrels'));
  const result = evalRun(merged, qrels, [10,100,1000]);
  console.log(`queries=${result.evaluated} relevant=${result.relevantTotal}`);
  [10,100,1000].forEach((cutoff, i) => console.log(`Recall@${cutoff} macro=${(result.macro[i]/result.evaluated).toFixed(4)} micro=${(result.hits[i]/result.relevantTotal).toFixed(4)} hits=${result.hits[i]}/${result.relevantTotal}`));
}
