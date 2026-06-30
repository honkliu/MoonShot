#!/usr/bin/env python3
"""Persistent local BGE embedding service for MoonShot.

Protocol over TCP:
  request:  uint32 little-endian byte length + UTF-8 query bytes
  response: uint32 little-endian dim (=512) + int8[512]
"""

import argparse
import socketserver
import struct
import threading

import numpy as np


class BgeEmbedder:
    def __init__(self, model_name: str, max_length: int, prefix: str | None, no_default_prefix: bool):
        from sentence_transformers import SentenceTransformer

        if prefix is None and not no_default_prefix and "bge" in model_name.lower():
            prefix = "Represent this sentence for searching relevant passages: "
        elif prefix is None:
            prefix = ""
        self.prefix = prefix
        self.lock = threading.Lock()
        self.model = SentenceTransformer(model_name)
        if hasattr(self.model, "max_seq_length"):
            self.model.max_seq_length = max_length

    def embed_i8(self, text: str) -> bytes:
        with self.lock:
            vector = self.model.encode(
                [self.prefix + text],
                batch_size=1,
                normalize_embeddings=True,
                convert_to_numpy=True,
                show_progress_bar=False,
            )[0].astype(np.float32)
        if vector.shape[0] < 512:
            padded = np.zeros(512, dtype=np.float32)
            padded[: vector.shape[0]] = vector
            vector = padded
        elif vector.shape[0] > 512:
            vector = vector[:512]
            norm = np.linalg.norm(vector)
            if norm > 0:
                vector = vector / norm
        return np.clip(np.rint(vector * 128.0), -128, 127).astype(np.int8).tobytes(order="C")


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        length_bytes = self._read_exact(4)
        if not length_bytes:
            return
        (length,) = struct.unpack("<I", length_bytes)
        if length == 0 or length > 65536:
            self.request.sendall(struct.pack("<I", 0))
            return
        text = self._read_exact(length).decode("utf-8", errors="replace")
        vector = self.server.embedder.embed_i8(text)
        self.request.sendall(struct.pack("<I", 512))
        self.request.sendall(vector)

    def _read_exact(self, size: int) -> bytes:
        chunks = []
        remaining = size
        while remaining > 0:
            chunk = self.request.recv(remaining)
            if not chunk:
                return b""
            chunks.append(chunk)
            remaining -= len(chunk)
        return b"".join(chunks)


class ThreadingServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--model", default="BAAI/bge-small-en-v1.5")
    parser.add_argument("--max-length", type=int, default=512)
    parser.add_argument("--prefix", default=None)
    parser.add_argument("--no-default-prefix", action="store_true")
    args = parser.parse_args()

    embedder = BgeEmbedder(args.model, args.max_length, args.prefix, args.no_default_prefix)
    with ThreadingServer((args.host, args.port), Handler) as server:
        server.embedder = embedder
        print(f"BGE embedding service listening on {args.host}:{args.port} model={args.model}", flush=True)
        server.serve_forever()


if __name__ == "__main__":
    main()
