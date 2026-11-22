#!/usr/bin/env python3
"""Run `mo test` on every Mo tool in tools/ and every flagship service in services/mo-*/."""

import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SUMMARY_RE = re.compile(r"^(\d+) passed, (\d+) failed, (\d+) stopped$", re.M)


def main(argv):
    if len(argv) != 1:
        raise SystemExit("usage: check_programs.py MO")
    mo = pathlib.Path(argv[0]).resolve()
    programs = sorted([
        *ROOT.glob("tools/*.mo"),
        *ROOT.glob("tools/*/main.mo"),
        *ROOT.glob("services/mo-*/main.mo"),
        *ROOT.glob("examples/*/main.mo"),
    ])
    if len(programs) < 15:
        raise SystemExit(f"expected at least 15 programs, found {len(programs)}")

    failures = []
    total_passed = 0
    for program in programs:
        rel = program.relative_to(ROOT)
        result = subprocess.run(
            [str(mo), "test", str(program)],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=120,
            check=False,
        )
        match = SUMMARY_RE.search(result.stdout)
        passed = int(match.group(1)) if match else 0
        failed = int(match.group(2)) if match else -1
        stopped = int(match.group(3)) if match else -1
        line = match.group(0) if match else "(no test summary)"
        print(f"{rel}: {line}")
        if result.returncode != 0 or passed == 0 or failed != 0 or stopped != 0:
            failures.append(
                f"mo test {rel} exited {result.returncode}:\n{result.stdout}{result.stderr}"
            )
        else:
            total_passed += passed

    if failures:
        for failure in failures:
            print(failure, file=sys.stderr)
        raise SystemExit(f"{len(failures)} program test suite(s) failed")

    print(f"{len(programs)} programs passed mo test ({total_passed} tests)")


if __name__ == "__main__":
    main(sys.argv[1:])
