#!/usr/bin/env python3
"""Reject missing files and stale pointer files before packing or booting images."""

import argparse
from pathlib import Path
import sys


def check(paths):
    errors = []
    for path in paths:
        files = sorted(path.rglob("*")) if path.is_dir() else [path]
        for file in files:
            if file.is_dir():
                continue
            try:
                with file.open("rb") as stream:
                    header = stream.read(256)
            except OSError as error:
                errors.append(f"{file}: {error.strerror}")
                continue
            if not header:
                errors.append(f"{file}: empty input file")
            elif header.startswith(b"version https://git-lfs.github.com/spec/v1\n"):
                errors.append(f"{file}: stale pointer file, not the actual blob")
    return errors


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", type=Path)
    errors = check(parser.parse_args().paths)
    if errors:
        print("Runtime inputs are not ready:\n  " + "\n  ".join(errors), file=sys.stderr)
        if any("stale pointer file" in error for error in errors):
            print("Re-run ./run to download runtime inputs, or replace the listed "
                  "files with real blobs.", file=sys.stderr)
        sys.exit(1)
