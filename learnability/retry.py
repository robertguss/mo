#!/usr/bin/env python3
"""The first-retry numbers a round's report gives (learnability/protocol.md sections 7 and 8).

  python3 learnability/retry.py LOGS

LOGS is the folder `harness/session.py collect` fills, one folder per session. The script runs the
reference tally (bench/tools/tally/hidden/reference.py) over LOGS, and over each session alone,
and prints tally's table as it printed it, then two sets of numbers: for every error, as tally
counts them, and again without the codes the guide tells sessions to keep (E1103, E1150, and
E1400, which only `mo test` reports). The second set is the one measured against the targets,
85% fixed on the first retry and 95% within three. Each set gives:

  - how many errors started, how many a `mo fix` run moved to by_fix, and how many are rated;
  - the first-retry and within-three rates, each with its 95% Wilson interval (wilson.py);
  - the number of sessions, how many of the rated errors came from the session with the most,
    and the two rates again with that session left out.

Leaving out a code is exact: tally identifies an error by its code, file and function, so the
other codes' rows don't depend on it. Leaving out a session is exact too: tally counts each
session on its own and adds them up, which the script checks against the run over LOGS.

The exit status is 0 when the numbers are printed; 1 when they are printed but tally skipped a
line of output.jsonl that isn't JSON (tally's message is on standard error); and 2 for a usage
error or when the reference tally fails.
"""

import fractions
import os
import pathlib
import subprocess
import sys
import tempfile

here = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(here))
import wilson  # noqa: E402

REFERENCE = here.parent / "bench/tools/tally/hidden/reference.py"
HEADER = "code\terrors\tfirst_retry\twithin_three\tby_fix\tunfixed"
KEPT = ("E1103", "E1150", "E1400")
FIRST_RETRY_TARGET = fractions.Fraction(85, 100)
WITHIN_THREE_TARGET = fractions.Fraction(95, 100)


class Failed(Exception):
    pass


def run_tally(logs):
    """Runs the reference over logs: (stdout, stderr, status), or Failed when it didn't tally."""
    result = subprocess.run([sys.executable, "-I", "-B", str(REFERENCE), str(logs)],
                            capture_output=True, text=True, check=False, timeout=600)
    if result.returncode not in (0, 1):
        raise Failed(f"the reference tally exited with {result.returncode} on {logs}:\n"
                     f"{result.stderr}")
    try:
        rows = parse(result.stdout)
    except ValueError as error:
        # A traceback also exits 1, but prints no table.
        raise Failed(f"the reference tally printed no table for {logs} ({error}):\n"
                     f"{result.stderr}") from None
    return rows, result.stdout, result.stderr, result.returncode


def parse(text):
    """Tally's table as {code: [errors, first_retry, within_three, by_fix, unfixed]}."""
    lines = text.split("\n")
    if not lines or lines[0] != HEADER:
        raise ValueError("no header line")
    rows = {}
    for line in lines[1:]:
        fields = line.split("\t")
        if len(fields) != 6 or not all(f.isascii() and f.isdigit() for f in fields[1:]):
            raise ValueError(f"not a table line: {line!r}")
        counts = [int(f) for f in fields[1:]]
        if fields[0] == "total":
            if counts != total(rows):
                raise ValueError("the total line isn't the sum of the codes' lines")
            return rows
        rows[fields[0]] = counts
    raise ValueError("no total line")


def total(rows, leave_out=()):
    sums = [0, 0, 0, 0, 0]
    for code, counts in rows.items():
        if code not in leave_out:
            sums = [a + b for a, b in zip(sums, counts)]
    return sums


def rated(counts):
    """The errors a rate is over: those started, less those mo fix moved."""
    return counts[0] - counts[3]


def rate(k, n, target=None):
    if n == 0:
        return "no errors to rate"
    low, high = wilson.wilson(k, n)
    line = (f"{k}/{n} = {wilson.percent(k / n)}, "
            f"95% Wilson interval [{wilson.percent(low)}, {wilson.percent(high)}]")
    if target is not None:
        line += f"; target {wilson.percent(float(target))}: {wilson.verdict(k, n, target)}"
    return line


def report(title, whole, sessions, leave_out, targets):
    counts = total(whole, leave_out)
    n = rated(counts)
    lines = [title,
             f"  errors: {counts[0]} started, {counts[3]} moved to by_fix by a mo fix run, "
             f"{n} rated",
             f"  first retry: {rate(counts[1], n, targets[0])}",
             f"  within three: {rate(counts[2], n, targets[1])}"]
    per_session = [(name, total(rows, leave_out)) for name, rows in sessions]
    most = max((rated(c) for _, c in per_session), default=0)
    if not per_session:
        lines.append("  sessions: 0")
    elif most == 0:
        lines.append(f"  sessions: {len(per_session)}; none has a rated error")
    else:
        # Ties go to the session tally reads first, which is the one left out.
        top = [name for name, c in per_session if rated(c) == most]
        tied = f", tied with {', '.join(top[1:])}" if len(top) > 1 else ""
        lines.append(f"  sessions: {len(per_session)}; the most rated errors, {most} of {n}, "
                     f"came from session {top[0]}{tied}")
        rest = [a - b for a, b in zip(counts, next(c for name, c in per_session
                                                  if name == top[0]))]
        lines.append(f"  without session {top[0]}, first retry: {rate(rest[1], rated(rest))}")
        lines.append(f"  without session {top[0]}, within three: {rate(rest[2], rated(rest))}")
    return lines


def main(args):
    if len(args) != 1:
        raise Failed("usage: retry.py LOGS")
    logs = pathlib.Path(args[0])
    if not logs.is_dir():
        raise Failed(f"can't read {logs}")
    whole, table, stderr, status = run_tally(logs)
    sys.stderr.write(stderr)
    sessions = []
    with tempfile.TemporaryDirectory() as scratch:
        # The same order and the same test tally uses to find its sessions.
        for name in sorted(os.listdir(logs)):
            if os.path.isdir(logs / name):
                alone = pathlib.Path(scratch) / str(len(sessions))
                alone.mkdir()
                (alone / name).symlink_to((logs / name).resolve(), target_is_directory=True)
                sessions.append((name, run_tally(alone)[0]))
    for code in set(whole) | {code for _, rows in sessions for code in rows}:
        summed = [sum(rows.get(code, [0] * 5)[i] for _, rows in sessions) for i in range(5)]
        if summed != whole.get(code, [0] * 5):
            raise Failed(f"the sessions' tallies don't add up to the tally over {logs} for {code}")
    out = [f"tally ({REFERENCE.relative_to(here.parent)}) over {logs}:", table.rstrip("\n"), ""]
    out += report("All errors, as tally counts them:", whole, sessions, (), (None, None))
    out.append("")
    out += report(f"Without {', '.join(KEPT[:-1])} and {KEPT[-1]}, the codes the guide tells "
                  "sessions to keep (measured against the targets):", whole, sessions, KEPT,
                  (FIRST_RETRY_TARGET, WITHIN_THREE_TARGET))
    print("\n".join(out))
    return status


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except (Failed, OSError, subprocess.TimeoutExpired) as error:
        print(f"retry.py: {error}", file=sys.stderr)
        sys.exit(2)
