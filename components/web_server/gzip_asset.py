#!/usr/bin/env python3
"""Create a deterministic gzip asset for embedding in the firmware."""

import gzip
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} INPUT OUTPUT", file=sys.stderr)
        return 2

    source = Path(sys.argv[1])
    destination = Path(sys.argv[2])
    destination.parent.mkdir(parents=True, exist_ok=True)

    with source.open("rb") as input_file, destination.open("wb") as output_file:
        with gzip.GzipFile(
            filename="",
            mode="wb",
            fileobj=output_file,
            compresslevel=9,
            mtime=0,
        ) as compressed_file:
            compressed_file.write(input_file.read())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
