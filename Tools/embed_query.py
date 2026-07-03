#!/usr/bin/env python3
"""Embed one query with a SentenceTransformers bi-encoder for MoonShot.

Writes MoonShot i8bin format:
  magic MSVECI81
    uint32 dim=128 by default
  uint32 id_bytes
    fixed id bytes + int8[dim]
"""

import argparse
import struct
from pathlib import Path

import numpy as np

MAGIC = b"MSVECI81"


def embed_text(text: str, model_name: str, max_length: int, prefix: str | None, no_default_prefix: bool, output_dim: int) -> np.ndarray:
    from sentence_transformers import SentenceTransformer

    if prefix is None and not no_default_prefix and "bge" in model_name.lower():
        prefix = "Represent this sentence for searching relevant passages: "
    elif prefix is None:
        prefix = ""

    model = SentenceTransformer(model_name)
    if hasattr(model, "max_seq_length"):
        model.max_seq_length = max_length

    vector = model.encode(
        [prefix + text],
        batch_size=1,
        normalize_embeddings=True,
        convert_to_numpy=True,
        show_progress_bar=False,
    )[0].astype(np.float32)

    if vector.shape[0] < output_dim:
        padded = np.zeros(output_dim, dtype=np.float32)
        padded[: vector.shape[0]] = vector
        vector = padded
    elif vector.shape[0] > output_dim:
        vector = vector[:output_dim]
        norm = np.linalg.norm(vector)
        if norm > 0:
            vector = vector / norm
    return vector


def write_i8bin(vector: np.ndarray, output: Path, query_id: str, id_bytes: int) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    encoded_id = query_id.encode("utf-8")
    if len(encoded_id) >= id_bytes:
        raise ValueError(f"id too long for fixed id bytes ({id_bytes}): {query_id}")
    quantized = np.clip(np.rint(vector * 128.0), -128, 127).astype(np.int8)
    with output.open("wb") as handle:
        handle.write(MAGIC)
        handle.write(struct.pack("<II", int(vector.shape[0]), id_bytes))
        handle.write(encoded_id + b"\0" * (id_bytes - len(encoded_id)))
        handle.write(quantized.tobytes(order="C"))


def main() -> None:
    parser = argparse.ArgumentParser()
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--text")
    group.add_argument("--text-file")
    parser.add_argument("--output", required=True)
    parser.add_argument("--id", default="query")
    parser.add_argument("--model", default="BAAI/bge-small-en-v1.5")
    parser.add_argument("--max-length", type=int, default=512)
    parser.add_argument("--id-bytes", type=int, default=32)
    parser.add_argument("--prefix", default=None)
    parser.add_argument("--no-default-prefix", action="store_true")
    parser.add_argument("--output-dim", type=int, default=128)
    args = parser.parse_args()
    if args.output_dim <= 0:
        raise SystemExit("--output-dim must be positive")

    if args.text_file:
        text = Path(args.text_file).read_text(encoding="utf-8")
    else:
        text = args.text
    vector = embed_text(text.strip(), args.model, args.max_length, args.prefix, args.no_default_prefix, args.output_dim)
    write_i8bin(vector, Path(args.output), args.id, args.id_bytes)


if __name__ == "__main__":
    main()
