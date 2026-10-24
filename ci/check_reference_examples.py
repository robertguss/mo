#!/usr/bin/env python3
"""Check every Mo fence in the reference with the real toolchain.

Run from any directory: python3 ci/check_reference_examples.py MO
Each block is tried as written, with a module, then as a function body.
Complete programs also run, with empty stdin and no arguments in a temporary
folder. A bare fence immediately following (apart from blank lines) is exact
stdout, including spaces and line endings. No following fence means run-only.

D309 markers on the line immediately before a Mo fence:
  <!-- mo-example: fragment -->       incomplete context, explicitly skipped
  <!-- mo-example: error E0053 -->    no form may pass; one must report this code
Malformed markers and unterminated Mo/output fences fail the check.
"""

import json
import pathlib
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass

ROOT = pathlib.Path(__file__).resolve().parent.parent
MARKER = re.compile(r"<!-- mo-example: (fragment|error E[0-9]{4}) -->")
MODULE = re.compile(r"^\s*module ([A-Z][A-Za-z0-9]*(?:\.[A-Z][A-Za-z0-9]*)*)\s*(?:#.*)?$", re.M)
MAIN = re.compile(r"^\s*fn main\s*\(", re.M)


@dataclass
class Example:
    line: int
    source: str
    marker: str = ""
    expected: bytes | None = None
    problem: str = ""


def examples(document):
    lines = document.splitlines(keepends=True)
    fences = []
    i = 0
    while i < len(lines):
        opening = re.fullmatch(r" {0,3}(`{3,})([^`\r\n]*)\r?\n?", lines[i])
        if not opening:
            i += 1
            continue
        start = i
        i += 1
        while i < len(lines) and not re.fullmatch(
                r" {0,3}`{" + str(len(opening[1])) + r",}[ \t]*\r?\n?", lines[i]):
            i += 1
        fences.append((start, i, opening[2].strip(), "".join(lines[start + 1:i])))
        i += 1
    for n, (start, end, language, source) in enumerate(fences):
        if language != "mo":
            continue
        block = Example(start + 1, source)
        if end == len(lines):
            block.problem = "unterminated Mo fence"
        previous = lines[start - 1].strip() if start else ""
        if previous.startswith("<!-- mo-example:"):
            marker = MARKER.fullmatch(previous)
            if marker:
                block.marker = marker[1]
            else:
                block.problem = "invalid mo-example marker"
        if n + 1 < len(fences):
            following, finish, info, output = fences[n + 1]
            if not info and not "".join(lines[end + 1:following]).strip():
                block.expected = output.encode("utf-8")
                if finish == len(lines):
                    block.problem = "unterminated output fence"
        yield block


def check(binary, block, directory):
    """Return a failure message and whether a complete program was run."""
    module = MODULE.search(block.source)
    name = module[1] if module else "Example"
    # Reference 2.1: acronym boundary first, then lower-case/digit to capital.
    path = re.sub(r"([A-Z])([A-Z][a-z])", r"\1-\2", name)
    path = re.sub(r"([a-z0-9])([A-Z])", r"\1-\2", path).lower().replace(".", "/")
    source = directory / (path + ".mo")
    source.parent.mkdir(parents=True, exist_ok=True)
    has_main = MAIN.search(block.source) is not None
    forms = [("as written", block.source)]
    if not module:
        forms.append(("with module", "module Example\n" + block.source))
        if not has_main:
            forms.append(("function body", "module Example\nfn reference_example()\n" +
                          block.source + "end\n"))
    wanted = block.marker.removeprefix("error ") if block.marker else None
    failures = []
    for label, text in forms:
        source.write_bytes(text.encode("utf-8"))
        result = subprocess.run([binary, "check", "--json", str(source)], cwd=directory,
                                input=b"", capture_output=True, timeout=30)
        if result.returncode not in (0, 1) or result.stderr:
            return f"mo check exited {result.returncode}: {result.stderr[:200]!r}", False
        records = [json.loads(line) for line in result.stdout.splitlines()]
        if not records or records[-1].get("record") != "totals":
            return "mo check did not report diagnostic totals", False
        diagnostics = [record for record in records if record.get("record") == "diagnostic"]
        totals = records[-1]
        if result.returncode == 0 and not diagnostics and totals["errors"] == 0:
            if wanted:
                return f"expected {wanted}, but checked {label}", False
            if not has_main:
                if block.expected is not None:
                    return "output fence requires a complete program with main", False
                return "", False
            result = subprocess.run([binary, "run", str(source)], cwd=directory,
                                    input=b"", capture_output=True, timeout=30)
            if result.returncode != 0:
                return f"mo run exited {result.returncode}: {result.stderr[:200]!r}", True
            if block.expected is not None and result.stdout != block.expected:
                return (f"output mismatch: expected {block.expected!r}, got {result.stdout!r}", True)
            return "", True
        failures.append(diagnostics)
    if wanted and any(d["code"] == wanted for ds in failures for d in ds):
        return "", False
    # Missing-module errors and nested-declaration errors add no useful context.
    best = failures[0] if module else failures[1]
    if best and best[0]["code"] == "E0031":
        best = failures[-1]
    diagnostic = f"{best[0]['code']}: {best[0]['headline']}" if best else "no diagnostic"
    return (f"expected {wanted}; {diagnostic}" if wanted else diagnostic), False


def main(argv):
    if len(argv) != 1:
        raise SystemExit("usage: check_reference_examples.py MO")
    binary = str(pathlib.Path(argv[0]).resolve())
    blocks = list(examples((ROOT / "docs/reference/README.md").read_bytes().decode("utf-8")))
    failed = checked = ran = skipped = 0
    for block in blocks:
        problem = block.problem
        if not problem and block.marker == "fragment":
            skipped += 1
            continue
        checked += 1
        if not problem:
            try:
                with tempfile.TemporaryDirectory(prefix="mo-reference-") as directory:
                    problem, did_run = check(binary, block, pathlib.Path(directory))
                    ran += did_run
            except (OSError, ValueError, subprocess.TimeoutExpired) as error:
                problem = str(error)
        if problem:
            print(f"FAIL line {block.line}: {' '.join(problem.splitlines())}")
            failed += 1
    if not blocks:
        print("FAIL: no Mo examples found")
        failed += 1
    print(f"reference examples: {len(blocks)} blocks, {checked} checked, {ran} run, "
          f"{skipped} fragments skipped, {failed} failures")
    return int(failed != 0)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
