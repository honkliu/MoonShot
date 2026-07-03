#!/usr/bin/env python3
"""Persistent local BGE embedding service for MoonShot.

Protocol over TCP:
        request:  uint32 little-endian byte length + UTF-8 text bytes
                            optional prefix "__MOONSHOT_BGE_DOCUMENT__\n" selects document mode
    response: uint32 little-endian dim (=128 by default) + int8[dim]
"""

import argparse
import socketserver
import struct
import threading

import numpy as np

DOCUMENT_MARKER = "__MOONSHOT_BGE_DOCUMENT__\n"


class BgeEmbedder:
    def __init__(self, model_name: str, max_length: int, prefix: str | None, doc_prefix: str | None, no_default_prefix: bool, output_dim: int):
        from sentence_transformers import SentenceTransformer

        if prefix is None and not no_default_prefix and "bge" in model_name.lower():
            prefix = "Represent this sentence for searching relevant passages: "
        elif prefix is None:
            prefix = ""
        if doc_prefix is None:
            doc_prefix = ""
        self.query_prefix = prefix
        self.doc_prefix = doc_prefix
        self.output_dim = output_dim
        self.lock = threading.Lock()
        self.model = SentenceTransformer(model_name)
        if hasattr(self.model, "max_seq_length"):
            self.model.max_seq_length = max_length

    def embed_i8(self, text: str, is_document: bool) -> bytes:
        prefix = self.doc_prefix if is_document else self.query_prefix
        with self.lock:
            vector = self.model.encode(
                [prefix + text],
                batch_size=1,
                normalize_embeddings=True,
                convert_to_numpy=True,
                show_progress_bar=False,
            )[0].astype(np.float32)
        if vector.shape[0] < self.output_dim:
            padded = np.zeros(self.output_dim, dtype=np.float32)
            padded[: vector.shape[0]] = vector
            vector = padded
        elif vector.shape[0] > self.output_dim:
            vector = vector[:self.output_dim]
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
        is_document = text.startswith(DOCUMENT_MARKER)
        if is_document:
            text = text[len(DOCUMENT_MARKER):]
        vector = self.server.embedder.embed_i8(text, is_document)
        self.request.sendall(struct.pack("<I", self.server.embedder.output_dim))
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
    parser.add_argument("--doc-prefix", default=None)
    parser.add_argument("--no-default-prefix", action="store_true")
    parser.add_argument("--output-dim", type=int, default=128)
    args = parser.parse_args()
    if args.output_dim <= 0:
        raise SystemExit("--output-dim must be positive")

    embedder = BgeEmbedder(args.model, args.max_length, args.prefix, args.doc_prefix, args.no_default_prefix, args.output_dim)
    with ThreadingServer((args.host, args.port), Handler) as server:
        server.embedder = embedder
        print(f"BGE embedding service listening on {args.host}:{args.port} model={args.model} dim={args.output_dim}", flush=True)
        server.serve_forever()


if __name__ == "__main__":
    main()
