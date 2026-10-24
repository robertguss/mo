#!/usr/bin/env python3
"""Build an optimized Mo and record validated end-to-end command timings."""

import argparse
import datetime
import hashlib
import json
import os
import pathlib
import platform
import resource
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
HERE = pathlib.Path(__file__).resolve().parent
BINARY = ROOT / "build/bench-release/debug/mo"
FLAGS = "-O2 -g -pthread"
RUN_CASES = {
    "greet": "greet-sample",
    "wc": "sample-wc",
    "find": "sample-find",
    "expenses": "sample-expenses",
    "json-query": "sample-json-query-path",
    "todo": "sample-todo-list-empty",
    "inventory": "sample-inventory",
    "kv": "sample-kv",
}
BENCHMARKS = {
    "list-building": b"100000 4999950000\n",
    "map-updates": b"1000 100000 100\n",
    "text-processing": b"30000 130000\n",
}


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT, text=True).strip()


def fixture(case, name, default=b""):
    path = ROOT / "tests/golden" / case / name
    return path.read_bytes() if path.exists() else default


def measure(argv, cwd, stdin, expected):
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    start = time.perf_counter_ns()
    result = subprocess.run(argv, cwd=cwd, input=stdin, capture_output=True, timeout=60)
    elapsed = (time.perf_counter_ns() - start) / 1e9
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    if result.returncode != 0 or result.stderr or result.stdout != expected:
        raise RuntimeError(f"{argv}: exit {result.returncode}, stdout={result.stdout!r}, "
                           f"stderr={result.stderr!r}, expected stdout={expected!r}")
    return {
        "wall_s": elapsed,
        "user_s": after.ru_utime - before.ru_utime,
        "system_s": after.ru_stime - before.ru_stime,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runs", type=int, default=9)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    if args.runs < 5:
        parser.error("--runs must be at least 5")
    build = ["make", "-B", "-j4", "CC=gcc", "BUILD=build/bench-release",
             f"DEBUG={FLAGS}", "all"]
    subprocess.run(build, cwd=ROOT, stdout=sys.stderr, check=True)
    samples = sorted((ROOT / "examples").glob("*.mo"))
    if len(samples) != 12:
        raise RuntimeError(f"expected twelve samples, found {len(samples)}")
    jobs = []
    with tempfile.TemporaryDirectory(prefix="mo-bench-") as isolated:
        for sample in samples:
            program = str(sample.relative_to(ROOT))
            for command in ("check", "test"):
                expected = (b"" if command == "check" else
                            fixture(f"sample-{sample.stem}-tests", "stdout"))
                if command == "test" and not expected:
                    raise RuntimeError(f"missing test golden for {sample.name}")
                jobs.append((program, command, [program], ROOT, b"", expected))
            if sample.stem in RUN_CASES:
                case = RUN_CASES[sample.stem]
                arguments = fixture(case, "args").decode().splitlines()
                cwd = pathlib.Path(isolated) if sample.stem == "todo" else ROOT
                jobs.append((program, "run", [str(sample), *arguments], cwd,
                             fixture(case, "stdin"), fixture(case, "stdout")))
        for name, expected in BENCHMARKS.items():
            program = str((HERE / f"{name}.mo").relative_to(ROOT))
            jobs.append((program, "run", [program], ROOT, b"", expected))
        measurements = []
        for program, command, arguments, cwd, stdin, expected in jobs:
            argv = [str(BINARY), command, *arguments]
            measure(argv, cwd, stdin, expected)
            measurements.append({
                "program": program, "command": command, "arguments": arguments[1:],
                "cwd": "temporary empty directory" if cwd != ROOT else ".",
                "stdin": stdin.decode(), "expected_stdout": expected.decode(), "runs": [],
            })
        load_before = os.getloadavg()
        for round_number in range(args.runs):
            order = range(len(jobs)) if round_number % 2 == 0 else reversed(range(len(jobs)))
            for index in order:
                program, command, arguments, cwd, stdin, expected = jobs[index]
                measurements[index]["runs"].append(
                    measure([str(BINARY), command, *arguments], cwd, stdin, expected))
        load_after = os.getloadavg()
    for row in measurements:
        walls = [run["wall_s"] for run in row["runs"]]
        row["median_s"] = statistics.median(walls)
        row["min_s"] = min(walls)
        row["max_s"] = max(walls)
        print(f"{row['command']:5} {row['program']:45} "
              f"{row['median_s'] * 1000:9.3f} ms "
              f"[{row['min_s'] * 1000:.3f}, {row['max_s'] * 1000:.3f}]", file=sys.stderr)
    cpu_info = pathlib.Path("/proc/cpuinfo").read_text()
    cpu_model = next(line.split(":", 1)[1].strip() for line in cpu_info.splitlines()
                     if line.startswith("model name"))
    cgroup = pathlib.Path("/sys/fs/cgroup/cpu.max")
    result = {
        "recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "commit": git("rev-parse", "HEAD"), "git_status": git("status", "--porcelain"),
        "machine": {"platform": platform.platform(), "cpu": cpu_model,
                    "logical_cpus": os.cpu_count(),
                    "affinity_cpus": len(os.sched_getaffinity(0)),
                    "cpu_max": cgroup.read_text().strip() if cgroup.exists() else None,
                    "memory": pathlib.Path("/proc/meminfo").read_text().splitlines()[0],
                    "python": platform.python_version(),
                    "compiler": subprocess.check_output(["gcc", "--version"], text=True)
                    .splitlines()[0], "load_before": load_before, "load_after": load_after},
        "build_command": build, "binary": str(BINARY.relative_to(ROOT)),
        "binary_sha256": hashlib.sha256(BINARY.read_bytes()).hexdigest(),
        "source_sha256": {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in [pathlib.Path(__file__).resolve(), *sorted(HERE.glob("*.mo"))]},
        "method": {"clock": "perf_counter_ns wall time around subprocess.run",
                   "warmups_per_job": 1, "runs_per_job": args.runs,
                   "order": "serial rounds, alternating forward and reverse job order",
                   "validation": "every invocation exits 0, stderr empty, stdout exact",
                   "timeout_s": 60, "failures": 0,
                   "scope": "fresh process including startup, compilation, execution and output capture"},
        "measurements": measurements,
    }
    args.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
