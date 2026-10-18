#!/usr/bin/env python3
"""The session-recording harness: stands in for `mo` in a learnability session (protocol section 2).

    session.py start --mo BINARY --sha256 HASH SESSION [--work FOLDER]
    session.py collect LOGS SESSION...
    session.py check-log LOG

`start` checks the frozen binary's hash, copies it into SESSION, and writes SESSION/bin/mo, the
command the session runs as `mo`. Every `mo` the session runs is then recorded:

- SESSION/log/NNN: each `mo check`, `mo test` and `mo fix`, as the session log of
  bench/tools/tally/task.md (`command`, `output.jsonl`, `src/`);
- SESSION/runs/NNN: every `mo` command, `mo run` included, with its arguments, folder, exit
  status and the output the session saw.

`collect` copies each session's log to LOGS/<session's folder name>, the layout `tally` reads.
`check-log` says whether a log has that layout. See learnability/harness/README.md.
"""

import datetime
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

CONFIG = None  # set in the copy `start` writes to SESSION/bin/mo

LOGGED = ("check", "test", "fix")
LOG_FILES = ("command", "output.jsonl", "src")


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="milliseconds")


def write(path, data):
    with open(path, "wb" if isinstance(data, bytes) else "w", encoding=None if isinstance(
            data, bytes) else "utf-8") as f:
        f.write(data)


def claim(folder, final=None):
    """Makes the next numbered folder under `folder` and returns its path. With `final`, moves
    the contents of that prepared folder into it. Safe when several runs finish at once, since
    making a folder either claims its number or fails."""
    os.makedirs(folder, exist_ok=True)
    number = 1 + max((int(name) for name in os.listdir(folder) if name.isdigit()), default=0)
    while True:
        path = os.path.join(folder, f"{number:03d}")
        try:
            os.mkdir(path)
            break
        except FileExistsError:
            number += 1
    if final is not None:
        for name in sorted(os.listdir(final)):
            os.rename(os.path.join(final, name), os.path.join(path, name))
        os.rmdir(final)
    return path


def snapshot(work, dest):
    """Copies every .mo file under `work` to `dest`, keeping its path."""
    os.makedirs(dest)
    for root, dirs, files in os.walk(work):
        dirs.sort()
        for name in sorted(files):
            if name.endswith(".mo"):
                source = os.path.join(root, name)
                target = os.path.join(dest, os.path.relpath(source, work))
                os.makedirs(os.path.dirname(target), exist_ok=True)
                shutil.copyfile(source, target)


def inside(path, folder):
    return os.path.commonpath([os.path.abspath(path), os.path.abspath(folder)]) == \
        os.path.abspath(folder)


def from_work(args, cwd, work):
    """The arguments as they'd be typed in `work`: each one naming a file inside it is rewritten
    relative to it, so the JSON run's `file` fields match the paths in `src/`."""
    out = []
    for arg in args:
        path = os.path.join(cwd, arg)
        if not arg.startswith("-") and os.path.isfile(path) and inside(path, work):
            out.append(os.path.relpath(path, work))
        else:
            out.append(arg)
    return out


def status_of(returncode):
    return 128 - returncode if returncode < 0 else returncode


def record(config, argv):
    real, session, work = config["mo"], config["session"], config["work"]
    cwd = os.getcwd()
    command = argv[0] if argv else ""
    runs = claim(os.path.join(session, "runs"))
    started, clock = now(), time.monotonic()
    write(os.path.join(runs, "argv"), json.dumps(argv) + "\n")
    write(os.path.join(runs, "cwd"),
          (os.path.relpath(cwd, work) if inside(cwd, work) else cwd) + "\n")
    write(os.path.join(runs, "started"), started + "\n")

    pending = None
    if command in LOGGED:
        pending = tempfile.mkdtemp(prefix="run-", dir=os.path.join(session, ".pending"))
        write(os.path.join(pending, "command"), command + "\n")
        snapshot(work, os.path.join(pending, "src"))

    seen = subprocess.run([real, *argv], capture_output=True, check=False)
    write(os.path.join(runs, "stdout"), seen.stdout)
    write(os.path.join(runs, "stderr"), seen.stderr)
    write(os.path.join(runs, "status"), f"{status_of(seen.returncode)}\n")
    for stream, data in [(sys.stdout, seen.stdout), (sys.stderr, seen.stderr)]:
        try:
            stream.buffer.write(data)
            stream.flush()
        except BrokenPipeError:  # `mo guide | head`: the reader has gone, and the record goes on
            pass

    if pending is not None:
        keep = True
        if command == "fix":
            keep = seen.returncode != 2  # a mistake in the command itself fixes nothing
        else:
            rest = [arg for arg in argv[1:] if arg != "--json"]
            quiet = subprocess.run([real, command, "--json", *from_work(rest, cwd, work)],
                                   cwd=work, capture_output=True, stdin=subprocess.DEVNULL,
                                   check=False)
            write(os.path.join(pending, "output.jsonl"), quiet.stdout)
            try:
                first = json.loads(quiet.stdout.split(b"\n", 1)[0])
            except ValueError:
                first = None
            # A refused command prints no version record and checked nothing; logging it would
            # count as a run that fixed every error.
            keep = isinstance(first, dict) and first.get("record") == "version"
        if keep:
            log = claim(os.path.join(session, "log"), final=pending)
            write(os.path.join(runs, "log"), os.path.basename(log) + "\n")
        else:
            shutil.rmtree(pending)
    write(os.path.join(runs, "seconds"), f"{time.monotonic() - clock:.3f}\n")
    return status_of(seen.returncode)


def start(args):
    import argparse
    parser = argparse.ArgumentParser(prog="session.py start")
    parser.add_argument("--mo", required=True, help="the frozen mo binary")
    parser.add_argument("--sha256", required=True, help="its SHA-256, from freeze.md")
    parser.add_argument("--work", help="the session's empty folder (default SESSION/work)")
    parser.add_argument("session")
    options = parser.parse_args(args)
    got = sha256(options.mo)
    if got != options.sha256.lower():
        print(f"session.py: {options.mo} has SHA-256 {got}, not {options.sha256}", file=sys.stderr)
        return 1
    session = os.path.abspath(options.session)
    if os.path.exists(session) and os.listdir(session):
        print(f"session.py: {session} is not empty", file=sys.stderr)
        return 1
    work = os.path.abspath(options.work or os.path.join(session, "work"))
    if os.path.exists(work) and os.listdir(work):
        print(f"session.py: {work} is not empty", file=sys.stderr)
        return 1
    for folder in ("bin", ".mo", ".pending", "log", "runs"):
        os.makedirs(os.path.join(session, folder))
    os.makedirs(work, exist_ok=True)
    real = os.path.join(session, ".mo", "mo")
    shutil.copyfile(options.mo, real)
    os.chmod(real, 0o755)
    if sha256(real) != got:
        print(f"session.py: the copy of {options.mo} changed", file=sys.stderr)
        return 1
    config = {"mo": real, "session": session, "work": work}
    with open(os.path.abspath(__file__), encoding="utf-8") as f:
        source = f.read()
    marker = "CONFIG = None"
    assert source.count(marker) == 2 and source.startswith("#!")  # this line holds the other
    source = source.replace(marker, "CONFIG = " + repr(config), 1)
    source = f"#!{sys.executable} -I\n" + source.split("\n", 1)[1]
    wrapper = os.path.join(session, "bin", "mo")
    write(wrapper, source)
    os.chmod(wrapper, 0o755)
    write(os.path.join(session, "session.json"),
          json.dumps({"mo_sha256": got, "work": work, "started": now()}, indent=2) + "\n")
    print(f"PATH={os.path.join(session, 'bin')}:$PATH")
    print(f"cd {work}")
    return 0


def check_log(log):
    """Returns the problems with the session log at `log`, as messages."""
    problems = []
    names = sorted(os.listdir(log))
    numbers = [name for name in names if name.isdigit() and len(name) == 3]
    for name in names:
        if name not in numbers:
            problems.append(f"{name}: not a run's folder")
    expected = [f"{n:03d}" for n in range(1, len(numbers) + 1)]
    if numbers != expected:
        problems.append(f"runs are not numbered 001 to {len(numbers):03d}")
    for name in numbers:
        run = os.path.join(log, name)
        try:
            with open(os.path.join(run, "command"), encoding="utf-8") as f:
                command = f.read()
        except OSError:
            problems.append(f"{name}: no command")
            continue
        if command not in ("check\n", "test\n", "fix\n"):
            problems.append(f"{name}: command is {command!r}")
            continue
        if not os.path.isdir(os.path.join(run, "src")):
            problems.append(f"{name}: no src/")
        if command == "fix\n":
            continue
        try:
            with open(os.path.join(run, "output.jsonl"), encoding="utf-8") as f:
                lines = f.read().splitlines()
            records = [json.loads(line) for line in lines]
        except (OSError, ValueError) as e:
            problems.append(f"{name}: output.jsonl: {e}")
            continue
        if not records or records[0].get("record") != "version" or \
                records[0].get("command") != command.strip():
            problems.append(f"{name}: output.jsonl doesn't start with a {command.strip()} version")
        if not records or records[-1].get("record") != "totals":
            problems.append(f"{name}: output.jsonl doesn't end with totals")
    return problems


def collect(args):
    if len(args) < 2:
        print("usage: session.py collect LOGS SESSION...", file=sys.stderr)
        return 2
    logs = args[0]
    os.makedirs(logs, exist_ok=True)
    for session in args[1:]:
        target = os.path.join(logs, os.path.basename(os.path.abspath(session)))
        shutil.copytree(os.path.join(session, "log"), target)
    return 0


def main(argv):
    if CONFIG is not None:
        return record(CONFIG, argv)
    if argv[:1] == ["start"]:
        return start(argv[1:])
    if argv[:1] == ["collect"]:
        return collect(argv[1:])
    if argv[:1] == ["check-log"] and len(argv) == 2:
        problems = check_log(argv[1])
        for problem in problems:
            print(f"{argv[1]}: {problem}", file=sys.stderr)
        return 1 if problems else 0
    print(__doc__.split("\n\n")[1], file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
