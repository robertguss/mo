#!/usr/bin/env python3
"""Check the real formatter on every tracked .mo file, without rewriting the corpus.

Run from any directory: python3 ci/check_format.py MO...
Invalid syntax must be refused without a rewrite. Accepted files must reach a
fixed point after one pass, preserve checker diagnostics apart from locations,
and preserve execution results. Runs use empty stdin and arguments in separate
capability roots seeded with golden data. Golden cases also use recorded inputs.
"""

import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
from collections import Counter

ROOT = pathlib.Path(__file__).resolve().parent.parent
POSITIONS = {"start", "end", "line", "column", "end_line", "end_column"}


def diagnostic(value):
    if isinstance(value, dict):
        return {k: diagnostic(v) for k, v in value.items() if k not in POSITIONS}
    if isinstance(value, list):
        return [diagnostic(v) for v in value]
    return value


def invoke(binary, folder, *args, stdin=b""):
    result = subprocess.run([str(binary), *map(str, args)], cwd=folder,
                            input=stdin, capture_output=True, timeout=30)
    assert result.returncode >= 0, f"{args}: signal {-result.returncode}"
    return result


def check(binary, folder, path):
    result = invoke(binary, folder, "check", "--json", path)
    records = [diagnostic(json.loads(line)) for line in result.stdout.splitlines()]
    assert records and records[-1]["record"] == "totals", (path, result)
    assert not result.stderr, (path, result.stderr)
    return result.returncode, records


def test_output(output, folder):
    return re.sub(rb"(?m)^((?:passed|failed|stopped|  at) [^\n]+  )(?:" +
                  re.escape(bytes(folder) + b"/") + rb")?([^\n]+\.mo):\d+:\d+$",
                  rb"\1\2:<position>", output)


def execute(binary, folder, path, command="run", arguments=(), stdin=b""):
    with tempfile.TemporaryDirectory(prefix="mo-format-run-") as work:
        for data in (folder / "tests/golden").glob("*/data"):
            shutil.copytree(data, pathlib.Path(work) / data.relative_to(folder), symlinks=True)
        result = invoke(binary, work, command, folder / path, *arguments, stdin=stdin)
        written = {str(p.relative_to(work)): p.read_bytes()
                   for p in pathlib.Path(work).rglob("*") if p.is_file()}
    if command == "run" and result.stderr.startswith(b"error E0079:"):
        assert result.returncode == 1 and not result.stdout
        return None
    # Bug stack frames carry byte offsets, not program output.
    errors = re.sub(rb"(?m)^(  at [^\n]*\.mo:)\d+\.\.\d+(\))$",
                    rb"\1<span>\2", result.stderr)
    output = test_output(result.stdout, folder) if command == "test" else result.stdout
    return result.returncode, output, errors, written


def audit(binary, tracked):
    programs = [p for p in tracked if p.suffix == ".mo"]
    subprocess.run([str(binary.parent / "tests/test_format.test"), "--files",
                    *map(str, programs)], cwd=ROOT, check=True)
    counts = Counter()
    failures = []
    with tempfile.TemporaryDirectory(prefix="mo-format-corpus-") as work:
        folder = pathlib.Path(work)
        for path in tracked:
            destination = folder / path
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / path, destination, follow_symlinks=False)
        before = {}
        runs = {}
        for path in programs:
            before[path] = check(binary, folder, path)
            if before[path][0] == 0:
                runs[path] = execute(binary, folder, path)
        golden = []
        for case in sorted((folder / "tests/golden").iterdir()):
            if not case.is_dir():
                continue
            def read(name, default=b""):
                file = case / name
                return file.read_bytes() if file.exists() else default
            path = pathlib.Path(read("entry").decode().strip()) if (case / "entry").exists() \
                else case.relative_to(folder) / "main.mo"
            command = read("command", b"run").decode().strip()
            arguments = read("args").decode().splitlines()
            stdin = read("stdin")
            expected = execute(binary, folder, path, command, arguments, stdin)
            stdout = test_output(read("stdout"), folder) if command == "test" else read("stdout")
            assert expected is not None and expected[:2] == \
                (int(read("status", b"0")), stdout), \
                f"golden {case.name}: baseline did not match recorded status/stdout: {expected!r}"
            golden.append((case.name, path, command, arguments, stdin, expected))
        for path in programs:
            source = (folder / path).read_bytes()
            first = invoke(binary, folder, "format", path)
            once = (folder / path).read_bytes()
            second = invoke(binary, folder, "format", path)
            if (first.returncode, first.stdout, first.stderr) != \
                    (second.returncode, second.stdout, second.stderr) or \
                    once != (folder / path).read_bytes():
                failures.append(f"{path}: format is not idempotent")
            if first.returncode:
                counts["refused"] += 1
                if once != source:
                    failures.append(f"{path}: refused format changed the file")
                if before[path][0] == 0:
                    failures.append(f"{path}: format refused a checked program\n{first.stderr!r}")
            else:
                counts["formatted"] += 1
        for path in programs:
            after = check(binary, folder, path)
            if before[path] != after:
                failures.append(f"{path}: check changed\n{before[path]!r}\n{after!r}")
            if path in runs:
                result = execute(binary, folder, path)
                counts["no main" if runs[path] is None else "runs"] += 1
                if runs[path] != result:
                    failures.append(f"{path}: run changed\n{runs[path]!r}\n{result!r}")
        for name, path, command, arguments, stdin, expected in golden:
            result = execute(binary, folder, path, command, arguments, stdin)
            if expected != result:
                failures.append(f"golden {name}: {command} changed\n{expected!r}\n{result!r}")
        assert not failures, "\n\n".join(failures)
    print(f"{binary}: {len(programs)} tracked .mo files, {counts['formatted']} formatted, "
          f"{counts['refused']} unchanged refusals, {counts['runs']} run comparisons, "
          f"{counts['no main']} modules without main, "
          f"{len(golden)} golden input comparisons")


def main():
    assert len(sys.argv) > 1, "usage: check_format.py MO..."
    tracked = [pathlib.Path(p.decode()) for p in subprocess.check_output(
        ["git", "ls-files", "-z"], cwd=ROOT).split(b"\0") if p]
    for binary in sys.argv[1:]:
        audit(pathlib.Path(binary).resolve(), tracked)


if __name__ == "__main__":
    main()
