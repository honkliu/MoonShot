#!/usr/bin/env python3
import argparse
import json
from pathlib import Path


def load_qrels_map(qrels_path):
    qrels = {}
    with qrels_path.open('r', encoding='utf-8') as handle:
        first = True
        for line in handle:
            if first:
                first = False
                if 'query-id' in line:
                    continue
            cols = line.rstrip('\n').split('\t')
            if len(cols) < 3:
                continue
            try:
                score = float(cols[2])
            except ValueError:
                continue
            if score > 0:
                qrels.setdefault(cols[0], set()).add(cols[1])
    return qrels


def load_qids(query_path, qrels, limit):
    qids = []
    with query_path.open('r', encoding='utf-8') as handle:
        for line in handle:
            if not line.strip():
                continue
            row = json.loads(line)
            qid = str(row['_id'])
            if qid not in qrels:
                continue
            qids.append(qid)
            if limit and len(qids) >= limit:
                break
    return set(qids)


def load_relevant(qrels_path, qids):
    relevant = set()
    with qrels_path.open('r', encoding='utf-8') as handle:
        first = True
        for line in handle:
            if first:
                first = False
                if 'query-id' in line:
                    continue
            cols = line.rstrip('\n').split('\t')
            if len(cols) < 3 or cols[0] not in qids:
                continue
            try:
                score = float(cols[2])
            except ValueError:
                continue
            if score > 0:
                relevant.add(cols[1])
    return relevant


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--data', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--query-limit', type=int, default=300)
    parser.add_argument('--distractors', type=int, default=5000)
    args = parser.parse_args()

    data = Path(args.data)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    qrels = load_qrels_map(data / 'qrels' / 'dev.tsv')
    qids = load_qids(data / 'queries.jsonl', qrels, args.query_limit)
    relevant = {docid for qid in qids for docid in qrels.get(qid, set())}
    wanted = set(relevant)

    distractors = 0
    with (data / 'corpus.jsonl').open('r', encoding='utf-8') as src, (out / 'corpus.jsonl').open('w', encoding='utf-8') as dst:
        for line in src:
            if not line.strip():
                continue
            row = json.loads(line)
            docid = str(row['_id'])
            if docid in wanted:
                dst.write(line)
            elif distractors < args.distractors:
                dst.write(line)
                distractors += 1
    with (data / 'queries.jsonl').open('r', encoding='utf-8') as src, (out / 'queries.jsonl').open('w', encoding='utf-8') as dst:
        kept = 0
        for line in src:
            if not line.strip():
                continue
            row = json.loads(line)
            if str(row['_id']) in qids:
                dst.write(line)
                kept += 1
    (out / 'qrels').mkdir(exist_ok=True)
    with (data / 'qrels' / 'dev.tsv').open('r', encoding='utf-8') as src, (out / 'qrels' / 'dev.tsv').open('w', encoding='utf-8') as dst:
        for line in src:
            cols = line.rstrip('\n').split('\t')
            if len(cols) >= 3 and (cols[0] == 'query-id' or (cols[0] in qids and cols[1] in wanted)):
                dst.write(line)
    print(f'qids={len(qids)} relevant_docs={len(relevant)} distractors={distractors} out={out}')


if __name__ == '__main__':
    main()
