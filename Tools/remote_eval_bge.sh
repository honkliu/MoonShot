#!/usr/bin/env bash
set -euo pipefail
cd "$HOME/moonshot_bge"
source .venv-bge/bin/activate
rm -f vectors/bge-vector-full.part*.trec logs/gpu-vector-part*.log
SHARDS=7
TOTAL=6980
CHUNK=$(( (TOTAL + SHARDS - 1) / SHARDS ))
for SHARD in $(seq 0 $((SHARDS-1))); do
  GPU=$((SHARD+1))
  START=$((SHARD*CHUNK))
  CUDA_VISIBLE_DEVICES=$GPU nohup python tools/gpu_vector_eval.py \
    --docs vectors/msmarco-docs-bge-small.i8bin \
    --queries vectors/msmarco-queries-bge-small.i8bin \
    --query-jsonl data/msmarco/msmarco/queries.jsonl \
    --qrels data/msmarco/msmarco/qrels/dev.tsv \
    --run-out vectors/bge-vector-full.part${SHARD}.trec \
    --k 1000 \
    --device cuda:0 \
    --query-batch 64 \
    --doc-block 131072 \
    --query-start "$START" \
    --query-limit "$CHUNK" > logs/gpu-vector-part${SHARD}.log 2>&1 &
done
jobs -l
