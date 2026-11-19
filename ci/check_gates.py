#!/usr/bin/env python3
"""The full run of mo test on lib/, examples/ and the task queue, which fails on any
survivor, function that states no intent or run that is not the full gate, then the same run on
a copy of examples/wc.mo with a test removed, which must fail and name a survivor.

Run from any directory: python3 ci/check_gates.py MO
"""

import concurrent.futures
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import time

root = pathlib.Path(__file__).resolve().parent.parent
TIMEOUT = 3600


def gated(binary, path):
    start = time.monotonic()
    result = subprocess.run([binary, "test", "--json", str(path)], cwd=root,
                            capture_output=True, text=True, timeout=TIMEOUT)
    records = [json.loads(line) for line in result.stdout.splitlines()]
    problems = [f"{d['primary']['file']}:{d['primary']['line']}: {d['code']} "
                f"{d['primary']['label']}" for d in records if d["record"] == "diagnostic"]
    ran = bool(records) and records[-1]["record"] == "totals" and records[-1].get("gates") == "ran"
    return result.returncode, problems, ran, result.stderr, time.monotonic() - start


def programs():
    return [root / "services/task-queue/queue-tests.mo", *sorted((root / "lib").glob("*.mo")),
            *sorted((root / "examples").glob("*.mo"))]


def control(binary):
    source = (root / "examples/wc.mo").read_text()
    test = re.search(r"^test \"a total follows .*?^end\n", source, re.M | re.S)
    assert test, "examples/wc.mo has lost the test the control removes"
    with tempfile.TemporaryDirectory(prefix="mo-gates-") as directory:
        copy = pathlib.Path(directory) / "wc.mo"
        copy.write_text(source[:test.start()] + source[test.end():])
        status, problems, _, stderr, _ = gated(binary, copy)
    assert status == 1 and not stderr, (status, stderr)
    assert problems and all(" E1439 " in p for p in problems), problems
    print(f"control: examples/wc.mo without its total test fails with {len(problems)} survivors")


def main(argv):
    if len(argv) != 1:
        raise SystemExit("usage: check_gates.py MO")
    binary = str(pathlib.Path(argv[0]).resolve())
    paths = programs()
    failed = []
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 2) as pool:
        for path, (status, problems, ran, stderr, seconds) in zip(
                paths, pool.map(lambda p: gated(binary, p), paths)):
            name = path.relative_to(root)
            print(f"{name}: exit {status} in {seconds:.1f} s", flush=True)
            for problem in problems:
                print(f"  {problem}")
            if status != 0 or stderr or problems or not ran:
                print(stderr, end="", file=sys.stderr)
                failed.append(str(name))
    if failed:
        raise SystemExit(f"mo test failed: {', '.join(failed)}")
    print(f"all {len(paths)} programs passed mo test")
    control(binary)


if __name__ == "__main__":
    main(sys.argv[1:])
