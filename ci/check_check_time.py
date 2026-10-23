#!/usr/bin/env python3
"""`mo check` stays under 0.1 s on each of the twelve samples.

Run from the repository root: python3 ci/check_check_time.py [--limit SECONDS] MO

Times `mo check examples/<name>.mo` as wall-clock seconds, the best of RUNS runs per sample
(the best run is the least disturbed by the machine), prints one row per sample, and fails when
any best time is over the limit or any run does not exit 0. Give it the debug (-O0) build: the
Makefile has no release build, the debug build is what the other checks drive, and the sample
times are well under the limit there. A sanitizer build is slower and is not the target.
Run it alone, not beside other checks, so the timings are not inflated by load.
--limit is for the negative control: a tiny limit must make the script fail.
"""

import pathlib
import subprocess
import sys
import time

root = pathlib.Path(__file__).resolve().parent.parent
LIMIT = 0.1
RUNS = 5
MINIMUM = 12  # the twelve samples; fewer means the glob found the wrong folder


def best_time(binary, sample):
    best = None
    for _ in range(RUNS):
        started = time.perf_counter()
        result = subprocess.run([binary, "check", str(sample.relative_to(root))], cwd=root,
                                capture_output=True, timeout=60)
        elapsed = time.perf_counter() - started
        if result.returncode != 0:
            raise SystemExit(f"mo check {sample.name} exited {result.returncode}:\n"
                             f"{result.stdout.decode(errors='replace')}"
                             f"{result.stderr.decode(errors='replace')}")
        best = elapsed if best is None else min(best, elapsed)
    return best


def main(argv):
    limit = LIMIT
    if argv[:1] == ["--limit"]:
        limit = float(argv[1])
        argv = argv[2:]
    if len(argv) != 1:
        raise SystemExit("usage: check_check_time.py [--limit SECONDS] MO")
    binary = str(pathlib.Path(argv[0]).resolve())
    samples = sorted((root / "examples").glob("*.mo"))
    if len(samples) < MINIMUM:
        raise SystemExit(f"found {len(samples)} samples in examples/, expected {MINIMUM}")
    slow = []
    print(f"mo check, best of {RUNS} runs, limit {limit:.3f} s ({argv[0]})")
    for sample in samples:
        seconds = best_time(binary, sample)
        over = seconds > limit
        if over:
            slow.append(sample.name)
        print(f"  {sample.name:<16} {seconds:7.4f} s{'  OVER' if over else ''}")
    if slow:
        raise SystemExit(f"over {limit} s: {', '.join(slow)}")
    print(f"all {len(samples)} samples within {limit} s")


main(sys.argv[1:])
