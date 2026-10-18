#!/usr/bin/env python3
"""The reference retry tally: the task text's rules, written plainly. Hidden from sessions.

Usage: reference.py LOGS, run from the folder the tool would be run from.
"""

import json
import os
import sys

STATUS = 0


def usage(message):
    print(f"tally: {message}", file=sys.stderr)
    sys.exit(2)


def function_of(src, file, line):
    try:
        with open(os.path.join(src, file), encoding="utf-8") as f:
            lines = f.read().split("\n")
    except OSError:
        return "(module)"
    for text in reversed(lines[:line]):
        if text.startswith("fn "):
            return text[3:].split("(")[0].split("[")[0].strip()
        if text.startswith("test "):
            return text.strip()
    return "(module)"


def read_run(folder):
    """Returns (command, errors, totals), or None when the run is skipped."""
    global STATUS
    with open(os.path.join(folder, "command"), encoding="utf-8") as f:
        command = f.read().strip()
    if command == "fix":
        return command, set(), None
    errors, totals = set(), None
    path = os.path.join(folder, "output.jsonl")
    with open(path, encoding="utf-8") as f:
        lines = f.read().split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    records = []
    for number, line in enumerate(lines, 1):
        try:
            records.append(json.loads(line))
        except ValueError:
            print(f"{path}:{number}: not JSON", file=sys.stderr)
            STATUS = 1
            return None
    for record in records:
        if not isinstance(record, dict):
            continue
        if record.get("record") == "diagnostic":
            span = record["primary"]
            errors.add((record["code"], span["file"],
                        function_of(os.path.join(folder, "src"), span["file"], span["line"])))
        elif record.get("record") == "totals" and command == "test":
            totals = (record["passed"], record["passed"] + record["failed"] + record["stopped"])
    return command, errors, totals


def tally_session(folder, counts):
    runs = sorted((name for name in os.listdir(folder) if name.isdigit()), key=int)
    sequence = [run for run in (read_run(os.path.join(folder, name)) for name in runs) if run]
    active = {}  # error -> attempts so far
    previous = set()
    fix_since = False
    last_totals = None
    for command, errors, totals in sequence:
        if command == "fix":
            fix_since = True
            continue
        if command == "test":
            last_totals = totals
        for error in list(active):
            active[error] += 1
            if error not in errors:
                row = counts.setdefault(error[0], [0, 0, 0, 0, 0])
                if fix_since:
                    row[3] += 1
                else:
                    row[1] += active[error] == 1
                    row[2] += active[error] <= 3
                del active[error]
        for error in errors:
            if error not in previous:
                counts.setdefault(error[0], [0, 0, 0, 0, 0])[0] += 1
                active[error] = 0
        previous = errors
        fix_since = False
    for error in active:
        counts[error[0]][4] += 1
    return last_totals


def percent(part, whole):
    if whole == 0:
        return "n/a"
    tenths = (2000 * part + whole) // (2 * whole)
    return f"{tenths // 10}.{tenths % 10}%"


def main():
    args = sys.argv[1:]
    if len(args) != 1:
        usage("usage: tally LOGS")
    logs = args[0]
    if not os.path.isdir(logs):
        usage(f"can't read {logs}")
    counts = {}
    passed = ran = 0
    for session in sorted(os.listdir(logs)):
        if os.path.isdir(os.path.join(logs, session)):
            totals = tally_session(os.path.join(logs, session), counts)
            if totals:
                passed += totals[0]
                ran += totals[1]
    print("code\terrors\tfirst_retry\twithin_three\tby_fix\tunfixed")
    total = [0, 0, 0, 0, 0]
    for code in sorted(counts):
        row = counts[code]
        print("\t".join([code, *map(str, row)]))
        total = [a + b for a, b in zip(total, row)]
    print("\t".join(["total", *map(str, total)]))
    print(f"first_retry_rate\t{percent(total[1], total[0] - total[3])}")
    print(f"within_three_rate\t{percent(total[2], total[0] - total[3])}")
    print(f"tests_passed\t{passed}/{ran} {percent(passed, ran)}")
    sys.exit(STATUS)


if __name__ == "__main__":
    main()
