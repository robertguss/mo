#!/usr/bin/env python3
"""Grades a program written for one of the three tools against the tool's hidden tests.

Run from anywhere:
  python3 bench/tools/grade.py TOOL PROGRAM.mo [--mo MO]   grade a session's program
  python3 bench/tools/grade.py TOOL --reference            run the tool's reference instead
  python3 bench/tools/grade.py --check                     every tool: SHA256SUMS match its
                                                           files, and its reference passes
  python3 bench/tools/grade.py --freeze TOOL               write TOOL's SHA256SUMS (once, when
                                                           the task is frozen)

TOOL is linkcheck, notioncopy or tally. Each test is a folder in TOOL/hidden/tests/ holding
  .input/ or input/   the folder the program runs in, copied fresh for each run (linkcheck's is
                      `.input/` so that linkcheck run over this repository skips it)
  input.tar.gz        instead, that folder as an archive
  empty-folders       folders to make, empty, inside that folder, one per line (git keeps none)
  args                the arguments after the program's file, one per line
  stdout              the exact standard output
  status              the exit status
  stderr-nonempty     when present, standard error must not be empty
  stderr-contains     when present, standard error must contain each of its lines
  after/              when present, exactly the files the folder must hold after the run
  seconds             when present, the run must finish within this many seconds (default 60)
Standard error is not otherwise compared. A program passes when every test passes.
"""

import hashlib
import os
import pathlib
import shutil
import subprocess
import sys
import tarfile
import tempfile

here = pathlib.Path(__file__).resolve().parent
TOOLS = ("linkcheck", "notioncopy", "tally")


def tree(folder):
    """Maps each file's path below folder to its bytes."""
    return {p.relative_to(folder).as_posix(): p.read_bytes()
            for p in sorted(folder.rglob("*")) if p.is_file() and not p.is_symlink()}


def prepare(case, work):
    if (case / "input.tar.gz").exists():
        work.mkdir()
        with tarfile.open(case / "input.tar.gz") as tar:
            options = {"filter": "tar"} if hasattr(tarfile, "tar_filter") else {}
            tar.extractall(work, **options)
    elif (case / ".input").exists() or (case / "input").exists():
        source = case / ".input" if (case / ".input").exists() else case / "input"
        shutil.copytree(source, work, symlinks=True)
    else:
        work.mkdir()
    if (case / "empty-folders").exists():
        for folder in (case / "empty-folders").read_text().splitlines():
            (work / folder).mkdir(parents=True)


def run_case(case, command):
    problems = []
    with tempfile.TemporaryDirectory() as temporary:
        work = pathlib.Path(temporary) / "work"
        prepare(case, work)
        args = (case / "args").read_text().splitlines() if (case / "args").exists() else []
        seconds = int((case / "seconds").read_text()) if (case / "seconds").exists() else 60
        try:
            result = subprocess.run([*command, *args], cwd=work, capture_output=True,
                                    timeout=seconds)
        except subprocess.TimeoutExpired:
            return [f"did not finish within {seconds} seconds"]
        expected = (case / "stdout").read_bytes()
        if result.stdout != expected:
            problems.append(f"stdout\n--- expected\n{expected.decode()}--- actual\n"
                            f"{result.stdout.decode(errors='replace')}")
        status = int((case / "status").read_text())
        if result.returncode != status:
            problems.append(f"status: expected {status}, got {result.returncode}")
        if (case / "stderr-nonempty").exists() and not result.stderr:
            problems.append("stderr: expected a message, got nothing")
        if (case / "stderr-contains").exists():
            for line in (case / "stderr-contains").read_text().splitlines():
                if line not in result.stderr.decode(errors="replace"):
                    problems.append(f"stderr: expected it to contain {line!r}, got "
                                    f"{result.stderr.decode(errors='replace')!r}")
        if (case / "after").exists():
            want, got = tree(case / "after"), tree(work)
            for path in sorted(set(want) | set(got)):
                if path not in got:
                    problems.append(f"after: {path} is missing")
                elif path not in want:
                    problems.append(f"after: {path} should not be there")
                elif want[path] != got[path]:
                    problems.append(f"after: {path} differs")
    return problems


def grade(tool, command):
    cases = sorted(p for p in (here / tool / "hidden/tests").iterdir() if p.is_dir())
    failed = 0
    for case in cases:
        problems = run_case(case, command)
        print(f"{'ok  ' if not problems else 'FAIL'} {tool}/{case.name}")
        for problem in problems:
            print("     " + problem.replace("\n", "\n     "))
        failed += bool(problems)
    print(f"{tool}: {len(cases) - failed} of {len(cases)} tests passed")
    return failed == 0


def sums(tool):
    folder = here / tool
    files = [folder / "task.md"] + sorted(p for p in (folder / "hidden").rglob("*")
                                          if p.is_file() and "__pycache__" not in p.parts)
    return "".join(f"{hashlib.sha256(p.read_bytes()).hexdigest()}  "
                   f"{p.relative_to(folder).as_posix()}\n" for p in files)


def reference(tool):
    return [sys.executable, "-I", "-B", str(here / tool / "hidden/reference.py")]


def main():
    args = sys.argv[1:]
    if args == ["--check"]:
        ok = True
        for tool in TOOLS:
            if (here / tool / "SHA256SUMS").read_text() != sums(tool):
                print(f"FAIL {tool}/SHA256SUMS does not match its files; frozen tasks don't "
                      f"change (see bench/tools/README.md)")
                ok = False
            ok = grade(tool, reference(tool)) and ok
        return 0 if ok else 1
    if len(args) == 2 and args[0] == "--freeze" and args[1] in TOOLS:
        (here / args[1] / "SHA256SUMS").write_text(sums(args[1]))
        return 0
    if len(args) == 2 and args[0] in TOOLS and args[1] == "--reference":
        return 0 if grade(args[0], reference(args[0])) else 1
    if len(args) in (2, 4) and args[0] in TOOLS and args[1].endswith(".mo"):
        mo = "mo"
        if len(args) == 4:
            if args[2] != "--mo":
                return usage()
            mo = str(pathlib.Path(args[3]).resolve())
        program = str(pathlib.Path(args[1]).resolve())
        return 0 if grade(args[0], [mo, "run", program]) else 1
    return usage()


def usage():
    print(__doc__.split("\n\n")[1], file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
