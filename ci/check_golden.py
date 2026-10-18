#!/usr/bin/env python3
"""The golden corpus: whole programs with their expected output and exit status.

Run from the repository root: python3 ci/check_golden.py MO...

tests/golden/ holds one folder per program. A folder has
  main.mo     the program (or `entry`, a file naming another program relative to the root, such
              as examples/wc.mo, so the twelve samples are in the corpus without copying them)
  command     `run` (the default) or `test`
  args        the arguments after the program's file, one per line
  stdin       what standard input holds (empty when absent)
  stdout      the exact standard output
  stderr      the exact standard error (empty when absent)
  status      the exit status (0 when absent)
The expected files are written by hand from the language reference, never copied from a run that
looks wrong: a difference is a bug in the program, in `mo`, or in the reference, and is reported.
"""

import pathlib
import re
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parent.parent
golden = root / "tests/golden"
MINIMUM = 100


def text(case, name, default=b""):
    path = case / name
    return path.read_bytes() if path.exists() else default


def run_case(binary, case):
    entry = (case / "entry").read_text().strip() if (case / "entry").exists() else None
    program = entry or str((case / "main.mo").relative_to(root))
    command = text(case, "command", b"run").decode().strip()
    assert command in ("run", "test"), case.name
    arguments = text(case, "args").decode().splitlines()
    result = subprocess.run([str(binary), command, program, *arguments], cwd=root,
                            input=text(case, "stdin"), capture_output=True, timeout=60)
    status = int(text(case, "status", b"0").decode().strip())
    problems = []
    if result.stdout != text(case, "stdout"):
        problems.append(f"stdout\n--- expected\n{text(case, 'stdout').decode()}\n--- actual\n"
                        f"{result.stdout.decode(errors='replace')}")
    if result.stderr != text(case, "stderr"):
        problems.append(f"stderr\n--- expected\n{text(case, 'stderr').decode()}\n--- actual\n"
                        f"{result.stderr.decode(errors='replace')}")
    if result.returncode != status:
        problems.append(f"status: expected {status}, got {result.returncode}")
    return entry, problems


def main():
    args = sys.argv[1:]
    only = [a[len("--case="):] for a in args if a.startswith("--case=")]
    binaries = [a for a in args if not a.startswith("--case=")]
    assert binaries, "usage: check_golden.py MO... [--case=NAME ...]"
    cases = sorted(p for p in golden.iterdir() if p.is_dir())
    if not only:
        assert len(cases) >= MINIMUM, f"the corpus has {len(cases)} programs, and needs {MINIMUM}"
    else:
        cases = [c for c in cases if c.name in only]
    failures = []
    for binary in binaries:
        samples = set()
        for case in cases:
            assert re.fullmatch(r"[a-z0-9]+(-[a-z0-9]+)*", case.name), case.name
            assert (case / "main.mo").exists() != (case / "entry").exists(), \
                f"{case.name}: exactly one of main.mo and entry"
            assert (case / "stdout").exists(), f"{case.name}: stdout is missing"
            entry, problems = run_case(binary, case)
            if entry and entry.startswith("examples/"):
                samples.add(entry)
            if problems:
                failures.append(f"{binary} {case.name}: " + "\n".join(problems))
        missing = set() if only else {f"examples/{p.name}" for p in (root / "examples").glob("*.mo")} - samples
        assert not missing, f"samples missing from the corpus: {sorted(missing)}"
    assert not failures, "\n\n".join(failures)
    print(f"{len(cases)} golden programs match")


main()
