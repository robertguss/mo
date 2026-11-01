#!/usr/bin/env python3
"""Run the three Mo-written tools in tools/ on this repository (MO-139).

Run from any directory: python3 ci/check_tools.py MO [--no-hidden]

For each tool, its own tests (`mo test`) and its frozen hidden tests (bench/tools/grade.py) must
pass. The hidden tests carry the frozen tasks' time limits, which were set for one gcc debug
build, so ci/check.sh runs them only with its first executable and passes --no-hidden for the
others (slower clang and sanitizer builds). Then each does its job here, as tools/README.md describes:

  - linkcheck over every tracked Markdown file: no broken link;
  - tally over each recorded learnability round's session logs: the same table, byte for byte,
    as the reference tally that learnability/retry.py runs. The logs are the release asset
    evidence-learnability-2026-10 (learnability/README.md says how to restore them); without
    them this comparison is skipped with a note;
  - notioncopy on docs/reference/README.md: the same pieces, byte for byte, as the reference.
"""

import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
NOTION_SOURCES = ["docs/reference/README.md"]


def run(command, cwd, timeout=120):
    return subprocess.run([str(c) for c in command], cwd=cwd, capture_output=True, text=True,
                          timeout=timeout, check=False)


def last(text, start=""):
    lines = [line for line in text.splitlines() if line.startswith(start)]
    return lines[-1] if lines else "(no output)"


def reference(tool):
    return [sys.executable, "-I", "-B", ROOT / f"bench/tools/{tool}/hidden/reference.py"]


def tests(mo, failures, hidden):
    for tool in ("linkcheck", "notioncopy", "tally"):
        program = TOOLS / f"{tool}.mo"
        result = run([mo, "test", program], ROOT)
        print(f"{tool}: mo test: {last(result.stdout)}")
        if result.returncode != 0:
            failures.append(f"mo test tools/{tool}.mo exited {result.returncode}:\n"
                            f"{result.stdout}{result.stderr}")
        if not hidden:
            continue
        result = run([sys.executable, ROOT / "bench/tools/grade.py", tool, program, "--mo", mo],
                     ROOT, timeout=600)
        print(f"{tool}: hidden tests: {last(result.stdout)}")
        if result.returncode != 0:
            failures.append(f"grade.py {tool} tools/{tool}.mo exited {result.returncode}:\n"
                            f"{result.stdout}{result.stderr}")


def linkcheck(mo, failures):
    """The repository's tracked files, Markdown whole and the rest empty (only existence counts),
    so untracked files and build output don't change the answer."""
    tracked = [p.decode() for p in subprocess.check_output(
        ["git", "ls-files", "-z"], cwd=ROOT).split(b"\0") if p]
    with tempfile.TemporaryDirectory(prefix="mo-linkcheck-") as work:
        for path in tracked:
            source = ROOT / path
            if not source.is_file():
                continue  # deleted in the work tree but not yet in the index
            destination = pathlib.Path(work) / path
            destination.parent.mkdir(parents=True, exist_ok=True)
            if path.endswith(".md"):
                shutil.copyfile(source, destination)
            else:
                destination.touch()
        result = run([mo, "run", TOOLS / "linkcheck.mo"], work)
    print(f"linkcheck over the repository: {last(result.stdout)}")
    if result.returncode != 0 or result.stderr:
        failures.append(f"linkcheck over the repository exited {result.returncode}:\n"
                        f"{result.stdout}{result.stderr}")


def tally(mo, failures):
    """Returns whether the comparison ran; without the session logs it is skipped with a note."""
    rounds = sorted(p for p in (ROOT / "learnability").glob("*/sessions") if p.is_dir())
    if not rounds:
        print("tally: skipped, no learnability/*/sessions folders here (restore them from the "
              "evidence-learnability-2026-10 release, see learnability/README.md)")
        return False
    for sessions in rounds:
        name = sessions.parent.relative_to(ROOT)
        with tempfile.TemporaryDirectory(prefix="mo-tally-") as work:
            collected = run([sys.executable, ROOT / "learnability/harness/session.py", "collect",
                             "logs", *sorted(p for p in sessions.iterdir() if p.is_dir())], work)
            if collected.returncode != 0:
                failures.append(f"session.py collect for {name}: {collected.stderr}")
                continue
            mine = run([mo, "run", TOOLS / "tally.mo", "logs"], work)
            theirs = run([*reference("tally"), "logs"], work)
        print(f"tally over {name}: {last(mine.stdout, 'total')}")
        if (mine.returncode, mine.stdout, mine.stderr) != (
                theirs.returncode, theirs.stdout, theirs.stderr):
            failures.append(f"tally over {name} differs from the reference:\n"
                            f"tools/tally.mo ({mine.returncode}):\n{mine.stdout}{mine.stderr}"
                            f"reference ({theirs.returncode}):\n{theirs.stdout}{theirs.stderr}")
    return True


def pieces(folder):
    return {p.name: p.read_bytes() for p in sorted(folder.iterdir())} if folder.is_dir() else {}


def notioncopy(mo, failures):
    for source in NOTION_SOURCES:
        with tempfile.TemporaryDirectory(prefix="mo-notioncopy-") as work:
            folder = pathlib.Path(work)
            (folder / source).parent.mkdir(parents=True)
            shutil.copyfile(ROOT / source, folder / source)
            mine = run([mo, "run", TOOLS / "notioncopy.mo", source, "mine"], work)
            theirs = run([*reference("notioncopy"), source, "theirs"], work)
            same = pieces(folder / "mine") == pieces(folder / "theirs")
        print(f"notioncopy on {source}: {len(mine.stdout.splitlines())} pieces")
        if mine.returncode != 0 or (mine.returncode, mine.stdout, mine.stderr) != (
                theirs.returncode, theirs.stdout, theirs.stderr) or not same:
            failures.append(f"notioncopy on {source} differs from the reference:\n"
                            f"tools/notioncopy.mo ({mine.returncode}):\n{mine.stdout}{mine.stderr}"
                            f"reference ({theirs.returncode}):\n{theirs.stdout}{theirs.stderr}"
                            f"pieces {'match' if same else 'differ'}")


def main(argv):
    if len(argv) not in (1, 2) or (len(argv) == 2 and argv[1] != "--no-hidden"):
        raise SystemExit("usage: check_tools.py MO [--no-hidden]")
    mo = pathlib.Path(argv[0]).resolve()
    failures = []
    tests(mo, failures, hidden=len(argv) == 1)
    linkcheck(mo, failures)
    tallied = tally(mo, failures)
    notioncopy(mo, failures)
    if failures:
        for failure in failures:
            print(failure, file=sys.stderr)
        raise SystemExit(f"{len(failures)} tool checks failed")
    print("the three tools pass their tests and do their jobs on this repository"
          + ("" if tallied else " (tally comparison skipped: no session logs)"))


if __name__ == "__main__":
    main(sys.argv[1:])
