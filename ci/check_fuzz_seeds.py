#!/usr/bin/env python3
"""Fail when a fuzz/fuzz_*.c has neither a committed corpus nor a Makefile
generated-seed branch. Run from the repository root: python3 ci/check_fuzz_seeds.py
"""

import pathlib
import re
import sys

root = pathlib.Path(__file__).resolve().parent.parent
makefile = (root / "Makefile").read_text()
joined = makefile.replace("\\\n", " ")
generated = set()
for part in re.split(r"\bif \[", joined):
    names = re.findall(r'\$\$base" = (\w+)', part) + re.findall(r'\$\$name" = (\w+)', part)
    if not names or "$$corpus" not in part:
        continue
    body = part.split("then", 1)[-1] if "then" in part else part
    if "rm -rf" in body and "cp " not in body:
        continue
    if " cp " in body or "\tcp " in body or ">" in body or "mkdir" in body:
        generated.update(names)

missing = []
for src in sorted((root / "fuzz").glob("fuzz_*.c")):
    name = src.stem
    corpus = root / "fuzz" / "corpus" / name
    has_corpus = corpus.is_dir() and any(corpus.iterdir())
    if not has_corpus and name not in generated:
        missing.append(name)

if missing:
    print(
        "fuzz target has neither a committed corpus nor a Makefile generated-seed branch:",
        ", ".join(missing),
        file=sys.stderr,
    )
    sys.exit(1)

if "tests/check/diagnostics/e*/*.mo" not in makefile:
    print(
        "Makefile fuzz-smoke must copy tests/check/diagnostics/e*/*.mo",
        file=sys.stderr,
    )
    sys.exit(1)
