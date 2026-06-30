#!/usr/bin/env python3
import argparse
import heapq
import json
import struct
from pathlib import Path

import numpy as np
import torch

MAGIC = b"MSVECI81"


def read_i8bin(path: Path):
    with path.open("rb") as handle:
        if handle.read(8) != MAGIC:
            raise ValueError(f"bad magic: {path}")
        dim, id_bytes = struct.unpack("<II", handle.read(8))
        payload = handle.read()
    record_size = id_bytes + dim
    if len(payload) % record_size != 0:
        raise ValueError(f"partial record payload: {path}")
    count = len(payload) // record_size
    ids = []
    vectors = np.empty((count, dim), dtype=np.int8)
    offset = 0
    for i in range(count):
        raw_id = payload[offset : offset + id_bytes]
        offset += id_bytes
        ids.append(raw_id.split(b"\0", 1)[0].decode("utf-8"))
        vectors[i] = np.frombuffer(payload, dtype=np.int8, count=dim, offset=offset)
        offset += dim
    return ids, vectors


def load_qrels(path: Path):
    qrels = {}
    with path.open("r", encoding="utf-8") as handle:
        first = True
        for line in handle:
            if first:
                first = False
                if "query-id" in line:
                    continue
            cols = line.rstrip("\n").split("\t")
            if len(cols) < 3:
                continue
            try:
                score = float(cols[2])
            except ValueError:
                continue
            if score > 0:
                qrels.setdefault(cols[0], set()).add(cols[1])
    return qrels


def query_ids_in_order(path: Path):
    ids = []
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            if line.strip():
                ids.append(str(json.loads(line)["_id"]))
    return ids


def evaluate_run(run, qrels, at):
    micro_hits = [0 for _ in at]
    micro_relevant = 0
    macro = [0.0 for _ in at]
    evaluated = 0
    for qid, docs in run.items():
        relevant = qrels.get(qid)
        if not relevant:
            continue
        hit_count = 0
        cumulative = [0 for _ in at]
        next_at = 0
        for rank, docid in enumerate(docs, start=1):
            if docid in relevant:
                hit_count += 1
            while next_at < len(at) and rank == at[next_at]:
                cumulative[next_at] = hit_count
                next_at += 1
        while next_at < len(at):
            cumulative[next_at] = hit_count
            next_at += 1
        micro_relevant += len(relevant)
        for i, value in enumerate(cumulative):
            micro_hits[i] += value
            macro[i] += value / len(relevant)
        evaluated += 1
    return evaluated, micro_relevant, micro_hits, macro


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--docs", required=True)
    parser.add_argument("--queries", required=True)
    parser.add_argument("--query-jsonl", required=True)
    parser.add_argument("--qrels", required=True)
    parser.add_argument("--run-out", required=True)
    parser.add_argument("--k", type=int, default=1000)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--query-start", type=int, default=0)
    parser.add_argument("--query-limit", type=int, default=0)
    parser.add_argument("--doc-block", type=int, default=131072)
    parser.add_argument("--query-batch", type=int, default=32)
    parser.add_argument("--device", default="cuda:0")
    args = parser.parse_args()

    doc_ids, doc_vectors = read_i8bin(Path(args.docs))
    query_vector_ids, query_vectors = read_i8bin(Path(args.queries))
    qid_order = query_ids_in_order(Path(args.query_jsonl))
    qrels = load_qrels(Path(args.qrels))
    if len(query_vector_ids) != len(qid_order):
        raise ValueError("query vector count does not match queries.jsonl")

    all_selected = []
    for index, qid in enumerate(qid_order):
        if qid not in qrels:
            continue
        all_selected.append(index)
        if args.limit > 0 and len(all_selected) >= args.limit:
            break
    selected = all_selected[args.query_start:]
    if args.query_limit > 0:
        selected = selected[:args.query_limit]

    device = torch.device(args.device)
    run = {}
    with Path(args.run_out).open("w", encoding="utf-8") as out:
        for batch_start in range(0, len(selected), args.query_batch):
            batch_indices = selected[batch_start : batch_start + args.query_batch]
            q = torch.from_numpy(query_vectors[batch_indices].astype(np.float32) / 128.0).to(device)
            q = torch.nn.functional.normalize(q, dim=1)
            heaps = [[] for _ in batch_indices]
            for doc_start in range(0, len(doc_ids), args.doc_block):
                block = doc_vectors[doc_start : doc_start + args.doc_block].astype(np.float32) / 128.0
                d = torch.from_numpy(block).to(device)
                d = torch.nn.functional.normalize(d, dim=1)
                scores = q @ d.T
                values, indices = torch.topk(scores, k=min(args.k, scores.shape[1]), dim=1)
                values = values.cpu().numpy()
                indices = indices.cpu().numpy()
                for row, heap in enumerate(heaps):
                    for score, local_index in zip(values[row], indices[row]):
                        global_index = doc_start + int(local_index)
                        item = (float(score), doc_ids[global_index])
                        if len(heap) < args.k:
                            heapq.heappush(heap, item)
                        elif item[0] > heap[0][0]:
                            heapq.heapreplace(heap, item)
            for qindex, heap in zip(batch_indices, heaps):
                qid = qid_order[qindex]
                ranked = sorted(heap, key=lambda item: (-item[0], item[1]))
                run[qid] = [docid for _, docid in ranked]
                for rank, (score, docid) in enumerate(ranked, start=1):
                    out.write(f"{qid} Q0 {docid} {rank} {score:.9g} bge-gpu\n")
            print(f"evaluated {min(batch_start + len(batch_indices), len(selected))}/{len(selected)}", flush=True)

    at = [10, 100, 1000]
    evaluated, relevant, hits, macro = evaluate_run(run, qrels, at)
    print(f"queries={evaluated} relevant={relevant}")
    for i, cutoff in enumerate(at):
        print(f"Recall@{cutoff} macro={macro[i] / evaluated:.4f} micro={hits[i] / relevant:.4f} hits={hits[i]}/{relevant}")


if __name__ == "__main__":
    main()
