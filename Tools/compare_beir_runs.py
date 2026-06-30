#!/usr/bin/env python3
import argparse
import csv
from collections import defaultdict
from pathlib import Path


def load_qrels(path: Path):
    qrels = defaultdict(set)
    with path.open("r", encoding="utf-8") as handle:
        reader = csv.reader(handle, delimiter="\t")
        for row in reader:
            if not row or row[0] == "query-id" or len(row) < 3:
                continue
            try:
                score = float(row[2])
            except ValueError:
                continue
            if score > 0:
                qrels[row[0]].add(row[1])
    return qrels


def load_run(path: Path, max_rank: int, qrels):
    run = defaultdict(dict)
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            parts = line.strip().split()
            if len(parts) < 6:
                continue
            qid, _, docid, rank_text, score_text, _tag = parts[:6]
            try:
                rank = int(rank_text)
                score = float(score_text)
            except ValueError:
                continue
            if docid not in qrels.get(qid, set()):
                continue
            if rank <= max_rank and docid not in run[qid]:
                run[qid][docid] = (rank, score)
    return run


def sorted_docs(docs, run):
    return sorted(docs, key=lambda docid: run.get(docid, (10**9, 0.0))[0])


def main():
    parser = argparse.ArgumentParser(description="Compare two BEIR/TREC run files against qrels.")
    parser.add_argument("--qrels", required=True)
    parser.add_argument("--left", required=True, help="Usually Lucene run")
    parser.add_argument("--right", required=True, help="Usually MoonShot run")
    parser.add_argument("--left-name", default="left")
    parser.add_argument("--right-name", default="right")
    parser.add_argument("--k", type=int, default=1000)
    parser.add_argument("--top", type=int, default=30, help="Number of worst delta queries to print")
    parser.add_argument("--out-prefix", default="")
    args = parser.parse_args()

    qrels = load_qrels(Path(args.qrels))
    left = load_run(Path(args.left), args.k, qrels)
    right = load_run(Path(args.right), args.k, qrels)
    qids = sorted(qid for qid in qrels if qid in left or qid in right)

    total_relevant = 0
    total_common = 0
    total_left_only = 0
    total_right_only = 0
    total_left_hits = 0
    total_right_hits = 0
    rows = []

    for qid in qids:
        relevant = qrels[qid]
        left_hits = set(left.get(qid, {})) & relevant
        right_hits = set(right.get(qid, {})) & relevant
        common = left_hits & right_hits
        left_only = left_hits - right_hits
        right_only = right_hits - left_hits
        delta = len(right_hits) - len(left_hits)

        total_relevant += len(relevant)
        total_common += len(common)
        total_left_only += len(left_only)
        total_right_only += len(right_only)
        total_left_hits += len(left_hits)
        total_right_hits += len(right_hits)

        rows.append({
            "qid": qid,
            "relevant": len(relevant),
            f"{args.left_name}_hits": len(left_hits),
            f"{args.right_name}_hits": len(right_hits),
            "common": len(common),
            f"{args.left_name}_only": len(left_only),
            f"{args.right_name}_only": len(right_only),
            "delta_right_minus_left": delta,
            f"{args.left_name}_only_docs": ";".join(sorted_docs(left_only, left.get(qid, {}))[:10]),
            f"{args.right_name}_only_docs": ";".join(sorted_docs(right_only, right.get(qid, {}))[:10]),
        })

    print(f"queries={len(qids)} k={args.k} relevant={total_relevant}")
    print(f"{args.left_name}_hits={total_left_hits} {args.right_name}_hits={total_right_hits} delta={total_right_hits - total_left_hits}")
    print(f"common={total_common} {args.left_name}_only={total_left_only} {args.right_name}_only={total_right_only}")
    if total_relevant:
        print(f"recall_{args.left_name}={total_left_hits / total_relevant:.6f} recall_{args.right_name}={total_right_hits / total_relevant:.6f}")

    worst_for_right = sorted(rows, key=lambda row: row["delta_right_minus_left"])[:args.top]
    best_for_right = sorted(rows, key=lambda row: row["delta_right_minus_left"], reverse=True)[:args.top]

    print("\nWorst right-minus-left queries:")
    for row in worst_for_right:
        print(row)

    print("\nBest right-minus-left queries:")
    for row in best_for_right:
        print(row)

    if args.out_prefix:
        output_path = Path(args.out_prefix + ".per_query.tsv")
        with output_path.open("w", encoding="utf-8", newline="") as handle:
            fieldnames = list(rows[0].keys()) if rows else ["qid"]
            writer = csv.DictWriter(handle, delimiter="\t", fieldnames=fieldnames)
            writer.writeheader()
            writer.writerows(rows)
        print(f"wrote {output_path}")


if __name__ == "__main__":
    main()
