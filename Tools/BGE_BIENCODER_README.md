# MoonShot BEIR Bi-Encoder Workflow

MoonShot stores document vectors directly in `DocDataEntry::DDE_VectorData` as a fixed 128-byte int8 payload. The production path is:

1. Generate normalized document/query embeddings with a real bi-encoder.
2. Build a MoonShot index with `-doc-vectors` so vectors are stored in DocData.
3. Evaluate vector-only, inverted-only, and hybrid modes.

Recommended starting model:

- `BAAI/bge-small-en-v1.5`
- Good quality/latency tradeoff, open model, 384 dimensions.
- The generator truncates and renormalizes to MoonShot's 128-dimensional slot by default.

Larger models such as `BAAI/bge-base-en-v1.5`, `BAAI/bge-large-en-v1.5`, or E5 can be used too, but dimensions above 128 are truncated unless the index format is expanded.

## Environment

Use a working Python environment. On this machine the global `python.exe` was broken, but `uv` successfully created a clean local environment:

```powershell
uv venv .venv-bge --python 3.11
uv pip install --python .\.venv-bge\Scripts\python.exe sentence-transformers torch numpy
```

## Generate Embeddings

```powershell
.\.venv-bge\Scripts\python.exe .\Tools\generate_beir_embeddings.py `
  --data .\build\beir\msmarco `
  --kind docs `
  --output .\build\beir\vectors\msmarco-docs-bge-small.tsv `
  --model BAAI/bge-small-en-v1.5 `
  --batch-size 64

.\.venv-bge\Scripts\python.exe .\Tools\generate_beir_embeddings.py `
  --data .\build\beir\msmarco `
  --kind queries `
  --output .\build\beir\vectors\msmarco-queries-bge-small.tsv `
  --model BAAI/bge-small-en-v1.5 `
  --batch-size 64
```

## Build Vector-Backed Index

```powershell
.\build\x64\Release\moon.exe `
  -idx .\build\beir\msmarco-full-v16-bge-small.idx `
  -beir-build `
  -data .\build\beir\msmarco `
  -doc-vectors .\build\beir\vectors\msmarco-docs-bge-small.tsv
```

## Evaluate Three Modes

Vector only:

```powershell
.\build\x64\Release\moon.exe `
  -idx .\build\beir\msmarco-full-v16-bge-small.idx `
  -beir-eval `
  -data .\build\beir\msmarco `
  -qrels dev `
  -k 10,100,1000 `
  -mode vector `
  -query-vectors .\build\beir\vectors\msmarco-queries-bge-small.tsv `
  -vector-ef 2000
```

Inverted only:

```powershell
.\build\x64\Release\moon.exe `
  -idx .\build\beir\msmarco-full-v16-bge-small.idx `
  -beir-eval `
  -data .\build\beir\msmarco `
  -qrels dev `
  -k 10,100,1000 `
  -streams TB `
  -mode weakand2phase
```

Hybrid:

```powershell
.\build\x64\Release\moon.exe `
  -idx .\build\beir\msmarco-full-v16-bge-small.idx `
  -beir-eval `
  -data .\build\beir\msmarco `
  -qrels dev `
  -k 10,100,1000 `
  -streams TB `
  -mode hybrid `
  -query-vectors .\build\beir\vectors\msmarco-queries-bge-small.tsv `
  -vector-ef 2000 `
  -vector-tail 128
```

Hybrid merges the inverted/rare-bigram result set with vector candidates, then reserves a configurable tail for vector-only candidates.

## Manual BGE Query With moon.exe

`moon.exe` can run a local CPU BGE query embedder and search the vectors already stored in `DocDataEntry::DDE_VectorData` through the existing HNSW/FreshDiskAnn path:

```powershell
.\build\x64\Release\moon.exe `
  -idx .\build\beir\msmarco-full-v16-bge-small.idx `
  -v `
  -bge `
  -k 20 `
  -ef 1000 `
  -bge-python .\.venv-bge\Scripts\python.exe `
  -bge-script .\Tools\embed_query.py
```

For hybrid manual search, include `-i` as well:

```powershell
.\build\x64\Release\moon.exe -idx .\build\beir\msmarco-full-v16-bge-small.idx -i -v -bge
```

Current implementation note: this uses a Python sidecar per query, so it loads the BGE model for each query. It is correct and local CPU based, but not low-latency production serving yet. On the 5,318-doc validation subset, startup built the HNSW graph in about 13.7s and two BGE queries completed in 35.5s total. The next production step is a persistent embedding sidecar or ONNX Runtime integration to avoid model reload per query.

## Validated Subset Smoke Test

For a qrels-covered 300-query subset with 5,318 documents (`300` dev queries, `318` relevant docs, `5000` distractors), `BAAI/bge-small-en-v1.5` vectors were generated and stored in MoonShot DocData without changing index format.

```text
vector-only:
  hits@10/100/1000 = 315 / 316 / 316

inverted weakand2phase:
  hits@10/100/1000 = 300 / 304 / 305

hybrid:
  hits@10/100/1000 = 301 / 315 / 318
```

This validates that real bi-encoder vectors are now part of the recall path. Full MS MARCO requires generating vectors for all corpus documents, which is a long-running offline job rather than an index-format change.
