#!/usr/bin/env python3
import argparse
import struct
from pathlib import Path

MAGIC = b"MSVECI81"


def read_header(handle):
    magic = handle.read(8)
    if magic != MAGIC:
        raise ValueError("bad vector magic")
    dim, id_bytes = struct.unpack("<II", handle.read(8))
    return dim, id_bytes


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    parser.add_argument("parts", nargs="+")
    args = parser.parse_args()

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    total_records = 0
    expected_dim = None
    expected_id_bytes = None
    with output.open("wb") as out:
        for index, part in enumerate(args.parts):
            path = Path(part)
            with path.open("rb") as handle:
                dim, id_bytes = read_header(handle)
                if expected_dim is None:
                    expected_dim = dim
                    expected_id_bytes = id_bytes
                    out.write(MAGIC)
                    out.write(struct.pack("<II", dim, id_bytes))
                elif dim != expected_dim or id_bytes != expected_id_bytes:
                    raise ValueError(f"header mismatch in {path}")
                payload = handle.read()
                record_size = id_bytes + dim
                if len(payload) % record_size != 0:
                    raise ValueError(f"partial record payload in {path}")
                total_records += len(payload) // record_size
                out.write(payload)
            print(f"appended {index}: {path} records={total_records}", flush=True)
    print(f"wrote {output} records={total_records}")


if __name__ == "__main__":
    main()
