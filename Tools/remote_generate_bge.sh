#!/usr/bin/env bash
set -euo pipefail
cd "$HOME/moonshot_bge"
mkdir -p data vectors logs tools
if [ ! -d data/msmarco ]; then
  python3 - <<'PY'
import zipfile
from pathlib import Path
zip_path = Path('data/msmarco.zip')
out = Path('data/msmarco')
out.mkdir(parents=True, exist_ok=True)
with zipfile.ZipFile(zip_path) as z:
    z.extractall(out)
print('unzipped', zip_path, 'to', out)
PY
fi
DATA_DIR="data/msmarco"
if [ -f data/msmarco/msmarco/corpus.jsonl ]; then
  DATA_DIR="data/msmarco/msmarco"
fi
if [ ! -d .venv-bge ]; then
  python3 -m venv .venv-bge
fi
source .venv-bge/bin/activate
python -m pip install --upgrade pip
python -m pip uninstall -y torch torchvision torchaudio nvidia-cublas nvidia-cuda-cupti nvidia-cuda-nvrtc nvidia-cuda-runtime nvidia-cudnn nvidia-cufft nvidia-cufile nvidia-curand nvidia-cusolver nvidia-cusparse nvidia-cusparselt nvidia-nccl nvidia-nvjitlink nvidia-nvshmem nvidia-nvtx || true
python -m pip install torch torchvision torchaudio --index-url https://download.pytorch.org/whl/cu124
python -m pip install sentence-transformers numpy
python tools/generate_beir_embeddings.py \
  --data "$DATA_DIR" \
  --kind queries \
  --output vectors/msmarco-queries-bge-small.i8bin \
  --model BAAI/bge-small-en-v1.5 \
  --batch-size 512 \
  --format i8bin
DOCS=$(DATA_DIR="$DATA_DIR" python - <<'PY'
import os
from pathlib import Path
print(sum(1 for _ in open(Path(os.environ['DATA_DIR']) / 'corpus.jsonl', encoding='utf-8')))
PY
)
SHARDS=7
CHUNK=$(( (DOCS + SHARDS - 1) / SHARDS ))
echo "docs=$DOCS chunk=$CHUNK"
for SHARD in $(seq 0 $((SHARDS-1))); do
  GPU=$(( SHARD + 1 ))
  START=$(( SHARD * CHUNK ))
  if [ "$START" -ge "$DOCS" ]; then continue; fi
  LIMIT=$CHUNK
  OUT="vectors/msmarco-docs-bge-small.part${SHARD}.i8bin"
  LOG="logs/embed-docs-part${SHARD}.log"
  if [ -s "$OUT" ]; then
    echo "skip existing $OUT"
    continue
  fi
  echo "launch gpu=$GPU start=$START limit=$LIMIT out=$OUT"
  CUDA_VISIBLE_DEVICES=$GPU nohup python tools/generate_beir_embeddings.py \
    --data "$DATA_DIR" \
    --kind docs \
    --start "$START" \
    --limit "$LIMIT" \
    --output "$OUT" \
    --model BAAI/bge-small-en-v1.5 \
    --batch-size 1024 \
    --format i8bin > "$LOG" 2>&1 &
done
jobs -l
