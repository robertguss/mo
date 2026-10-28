#!/usr/bin/env python3
"""Fail when a fuzz/fuzz_*.c has neither a committed corpus nor a Makefile
generated-seed branch. Run from the repository root: python3 ci/check_fuzz_seeds.py
"""

import pathlib
import re
import sys

root = pathlib.Path(__file__).resolve().parent.parent
# Drop Make comments so a commented-out copy or seed branch cannot satisfy the check.
active_lines = []
for line in (root / "Makefile").read_text().splitlines():
    code = line.split("#", 1)[0]
    if code.strip():
        active_lines.append(code)
makefile = "\n".join(active_lines)
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

# fuzz_bytecode_io_large is the Makefile large variant of fuzz_bytecode_io (no fuzz_*.c).
drivers = {src.stem for src in (root / "fuzz").glob("fuzz_*.c")}
allowed_corpus = {"fuzz_bytecode_io_large"}
orphan = []
for path in sorted((root / "fuzz" / "corpus").iterdir()):
    if path.is_dir() and path.name not in drivers and path.name not in allowed_corpus:
        orphan.append(path.name)
if orphan:
    print(
        "fuzz corpus directory has no matching fuzz/fuzz_*.c driver:",
        ", ".join(orphan),
        file=sys.stderr,
    )
    sys.exit(1)

# Both flat and nested diagnostic copies must be live in fuzz-smoke, and match files.
# Nested needs the for-glob and the cp rename; either alone can be commented out.
flat_glob = "tests/check/diagnostics/e*.mo"
nested_glob = "tests/check/diagnostics/e*/*.mo"
nested_copy = 'cp "$$path" "$$corpus/$$(basename $$(dirname $$path))-$$(basename $$path)"'
required = (
    (flat_glob, "flat diagnostics tests/check/diagnostics/e*.mo"),
    (nested_glob, "nested diagnostics tests/check/diagnostics/e*/*.mo"),
    (nested_copy, "nested diagnostic rename copy into the smoke corpus"),
)
for pattern, label in required:
    if pattern not in makefile:
        print(f"Makefile fuzz-smoke must include {label}", file=sys.stderr)
        sys.exit(1)
for pattern in (flat_glob, nested_glob):
    if not any(root.glob(pattern)):
        print(
            f"fuzz-smoke diagnostic glob matches no files: {pattern}",
            file=sys.stderr,
        )
        sys.exit(1)
