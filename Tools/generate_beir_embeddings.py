#!/usr/bin/env python3
"""Generate BEIR document/query embeddings for MoonShot.

Output format is TSV:
    external_id<TAB>v0,v1,...,v127 by default

Default model is BAAI/bge-small-en-v1.5 (384 dims). Vectors are L2-normalized
and padded/truncated to MoonShot's fixed 128-dim DocData vector slot.
"""

import argparse
import json
import struct
from pathlib import Path

import numpy as np


def iter_jsonl(path: Path):
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            if line.strip():
                yield json.loads(line)


def slice_rows(rows, start: int, limit: int):
    emitted = 0
    for index, row in enumerate(rows):
        if index < start:
            continue
        if limit > 0 and emitted >= limit:
            break
        yield row
        emitted += 1


def write_vectors(rows, output: Path, model_name: str, batch_size: int, max_length: int, id_field: str, text_fn, output_format: str, id_bytes: int, text_prefix: str, output_dim: int):
    from sentence_transformers import SentenceTransformer

    model = SentenceTransformer(model_name)
    if hasattr(model, "max_seq_length"):
        model.max_seq_length = max_length
    output.parent.mkdir(parents=True, exist_ok=True)
    done = 0
    mode = "wb" if output_format == "i8bin" else "w"
    open_kwargs = {} if output_format == "i8bin" else {"encoding": "utf-8"}
    with output.open(mode, **open_kwargs) as out:
        if output_format == "i8bin":
            out.write(b"MSVECI81")
            out.write(struct.pack("<II", output_dim, id_bytes))
        batch_ids = []
        batch_texts = []
        for row in rows:
            row_id = str(row[id_field])
            text = text_prefix + text_fn(row)
            batch_ids.append(row_id)
            batch_texts.append(text)
            if len(batch_ids) >= batch_size:
                done += flush_batch(model, batch_ids, batch_texts, out, output_format, id_bytes, output_dim)
                print(f"  embedded {done}", flush=True)
                batch_ids.clear()
                batch_texts.clear()
        if batch_ids:
            done += flush_batch(model, batch_ids, batch_texts, out, output_format, id_bytes, output_dim)
            print(f"  embedded {done}", flush=True)


def flush_batch(model, batch_ids, batch_texts, out, output_format: str, id_bytes: int, output_dim: int):
    embeddings = model.encode(
        batch_texts,
        batch_size=len(batch_texts),
        normalize_embeddings=True,
        convert_to_numpy=True,
        show_progress_bar=False,
    )
    embeddings = np.asarray(embeddings, dtype=np.float32)
    if embeddings.shape[1] < output_dim:
        padded = np.zeros((embeddings.shape[0], output_dim), dtype=np.float32)
        padded[:, : embeddings.shape[1]] = embeddings
        embeddings = padded
    elif embeddings.shape[1] > output_dim:
        embeddings = embeddings[:, :output_dim]
        norms = np.linalg.norm(embeddings, axis=1, keepdims=True)
        norms[norms == 0] = 1.0
        embeddings = embeddings / norms

    if output_format == "i8bin":
        quantized = np.clip(np.rint(embeddings * 128.0), -128, 127).astype(np.int8)
        for row_id, vector in zip(batch_ids, quantized):
            encoded_id = row_id.encode("utf-8")
            if len(encoded_id) >= id_bytes:
                raise ValueError(f"id too long for fixed id bytes ({id_bytes}): {row_id}")
            out.write(encoded_id + b"\0" * (id_bytes - len(encoded_id)))
            out.write(vector.tobytes(order="C"))
    else:
        for row_id, vector in zip(batch_ids, embeddings):
            values = ",".join(f"{float(value):.8g}" for value in vector)
            out.write(f"{row_id}\t{values}\n")
    return len(batch_ids)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--data", required=True, help="BEIR dataset directory")
    parser.add_argument("--output", required=True)
    parser.add_argument("--kind", choices=["docs", "queries"], required=True)
    parser.add_argument("--model", default="BAAI/bge-small-en-v1.5")
    parser.add_argument("--batch-size", type=int, default=64)
    parser.add_argument("--max-length", type=int, default=512)
    parser.add_argument("--format", choices=["tsv", "i8bin"], default="tsv")
    parser.add_argument("--id-bytes", type=int, default=32)
    parser.add_argument("--output-dim", type=int, default=128)
    parser.add_argument("--prefix", default=None, help="Optional text prefix before encoding. Defaults to BGE query instruction for BGE query embeddings.")
    parser.add_argument("--no-default-prefix", action="store_true", help="Disable model-specific default prefixes.")
    parser.add_argument("--start", type=int, default=0)
    parser.add_argument("--limit", type=int, default=0)
    args = parser.parse_args()
    if args.output_dim <= 0:
        raise SystemExit("--output-dim must be positive")

    data = Path(args.data)
    if args.prefix is not None:
        text_prefix = args.prefix
    elif not args.no_default_prefix and args.kind == "queries" and "bge" in args.model.lower():
        text_prefix = "Represent this sentence for searching relevant passages: "
    else:
        text_prefix = ""

    if args.kind == "docs":
        rows = slice_rows(iter_jsonl(data / "corpus.jsonl"), args.start, args.limit)
        write_vectors(
            rows,
            Path(args.output),
            args.model,
            args.batch_size,
            args.max_length,
            "_id",
            lambda row: ((row.get("title") or "") + " " + (row.get("text") or "")).strip(),
            args.format,
            args.id_bytes,
            text_prefix,
            args.output_dim,
        )
    else:
        rows = slice_rows(iter_jsonl(data / "queries.jsonl"), args.start, args.limit)
        write_vectors(
            rows,
            Path(args.output),
            args.model,
            args.batch_size,
            args.max_length,
            "_id",
            lambda row: str(row.get("text") or ""),
            args.format,
            args.id_bytes,
            text_prefix,
            args.output_dim,
        )


if __name__ == "__main__":
    main()
