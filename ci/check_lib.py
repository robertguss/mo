#!/usr/bin/env python3
"""Run every library file's examples, promises and tests through mo test.

Run from any directory: python3 ci/check_lib.py MO
"""

import pathlib
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parent.parent


def main(argv):
    if len(argv) != 1:
        raise SystemExit("usage: check_lib.py MO")
    binary = str(pathlib.Path(argv[0]).resolve())
    sources = sorted((root / "lib").rglob("*.mo"))
    if not sources:
        raise SystemExit("no library files found in lib/")
    failed = []
    for source in sources:
        path = str(source.relative_to(root))
        print(f"==> mo test {path}", flush=True)
        try:
            result = subprocess.run([binary, "test", path], cwd=root, timeout=60)
        except subprocess.TimeoutExpired:
            print(f"{path}: mo test timed out after 60 s", file=sys.stderr)
            failed.append(path)
            continue
        if result.returncode != 0:
            print(f"{path}: mo test exited {result.returncode}", file=sys.stderr)
            failed.append(path)
    if failed:
        raise SystemExit(f"library tests failed: {', '.join(failed)}")
    print(f"all {len(sources)} library files passed mo test")


if __name__ == "__main__":
    main(sys.argv[1:])
