#!/usr/bin/env python3
"""Grades a session's folder for one of benchmark v0's tasks, as learnability/protocol.md
section 3 says.

Run from anywhere:
  python3 bench/v0/grade.py TASK FOLDER [--mo MO] [--grammar MO_GRAMMAR]
                                          grade the folder a session left, such as its work/
  python3 bench/v0/grade.py TASK --reference [--mo MO]
                                          grade the task's reference solution instead
  python3 bench/v0/grade.py --check [--mo MO] [TASK...]
                                          every task (or those named): SHA256SUMS match its files,
                                          its reference passes, its broken solution and its start
                                          folder fail
  python3 bench/v0/grade.py --freeze TASK write TASK's SHA256SUMS (once, when it is frozen)

TASK is a task folder's name, such as 01-rle. MO defaults to `mo` on the PATH. Each task folder
holds
  task.md             the text the session reads
  task.json           {"kind": "write" | "fix" | "failing-test" | "extend", "held_out": BOOL,
                       "main": the main module's file, relative to the session's folder, and
                       for a failing-test task "failing_test": the full name of the one test
                       that fails in start/}
  start/              the files the session's folder starts with (every kind but write)
  hidden/reference/   the reference solution: the whole folder as its author left it
  hidden/broken/      a deliberately broken solution, which must fail a hidden test
  hidden/spec/        Mo test modules, copied over the session's files before they run, so
                      that editing or deleting a test doesn't help; each runs with
                      `mo test FILE --in MODULE`, and every test in it must pass
  hidden/runs/NAME/   runs of the program, `mo run MAIN ARGS...`, each a folder with
    input/              the folder the program runs in, copied fresh (else an empty one)
    args                the arguments after the main file, one per line
    stdin               standard input (else empty)
    stdout, status      the exact standard output, and the exit status
    stderr              when present, the exact standard error
    after/              when present, exactly the files the folder must hold after the run
  SHA256SUMS          the hash of every other file in the task folder

The verdict is the first step that fails: 1 parses (the executable grammar accepts every .mo
file; skipped without --grammar), 2 checks (`mo check MAIN` reports no diagnostic), 3 does the
task (every spec module and run passes). A folder whose only diagnostics are E1103 or E1150 may be
a toolchain gap, and its hidden tests still run; it is one only when each such diagnostic is on a
form the reference requires, which someone confirms by reading it, since this build also reports
E1103 for some mistakes.
"""

import concurrent.futures
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

here = pathlib.Path(__file__).resolve().parent
KINDS = ("write", "fix", "failing-test", "extend")
GAP_CODES = {"E1103", "E1150"}
SECONDS = 60


def tasks():
    return sorted(p.name for p in here.iterdir() if (p / "task.json").exists())


def meta(task):
    data = json.loads((here / task / "task.json").read_text())
    if data.get("kind") not in KINDS or not isinstance(data.get("held_out"), bool):
        raise SystemExit(f"{task}/task.json: kind or held_out is wrong")
    return data


def mo_files(folder):
    return sorted(p for p in folder.rglob("*.mo") if p.is_file())


def overlay(source, target):
    """Copies every file under source into target, replacing what is there. A link in target is
    replaced, never written through."""
    for path in sorted(source.rglob("*")):
        if path.is_file():
            destination = target / path.relative_to(source)
            destination.parent.mkdir(parents=True, exist_ok=True)
            if destination.is_symlink() or destination.exists():
                destination.unlink()
            shutil.copyfile(path, destination)


def tree(folder):
    return {p.relative_to(folder).as_posix(): p.read_bytes()
            for p in sorted(folder.rglob("*")) if p.is_file() and not p.is_symlink()}


def run(command, cwd, stdin=b""):
    try:
        return subprocess.run(command, cwd=cwd, input=stdin, capture_output=True,
                              timeout=SECONDS)
    except subprocess.TimeoutExpired:
        return None


def parses(folder, grammar):
    problems = []
    for path in mo_files(folder):
        result = run([grammar, str(path)], folder)
        if result is None or result.returncode != 0:
            problems.append(f"{path.relative_to(folder)}: the grammar rejects it")
    return problems


def diagnostics(folder, mo, main):
    """The codes of `mo check --json MAIN`'s diagnostics, with a line for each."""
    result = run([mo, "check", "--json", main], folder)
    if result is None:
        return [("timeout", "mo check did not finish")]
    found = []
    for line in result.stdout.decode(errors="replace").splitlines():
        record = json.loads(line)
        if record.get("record") == "diagnostic":
            place = record.get("primary") or {}
            found.append((record["code"], f"{record['code']} {record['headline']}  "
                          f"{place.get('file')}:{place.get('line')}"))
    if result.returncode != 0 and not found:
        found.append(("status", f"mo check exited {result.returncode} with no diagnostic"))
    return found


def module_name(path):
    match = re.search(r"^module\s+([A-Za-z0-9.]+)", path.read_text(), re.MULTILINE)
    if not match:
        raise SystemExit(f"{path}: no module line")
    return match.group(1)


def test_results(work, mo, relative, module):
    """Runs one test module; returns its exit status and the names of what didn't pass."""
    result = run([mo, "test", "--json", relative, "--in", module], work)
    if result is None:
        return None, [f"did not finish within {SECONDS} seconds"]
    failed = []
    for line in result.stdout.decode(errors="replace").splitlines():
        record = json.loads(line)
        if record.get("record") == "result" and record.get("outcome") != "passed":
            failed.append(f"{record['name']} {record['outcome']}")
        if record.get("record") == "diagnostic":
            failed.append(f"{record['code']} {record['headline']}")
    return result.returncode, failed


def spec_problems(task, folder, mo):
    spec = here / task / "hidden/spec"
    if not spec.exists():
        return []
    problems = []
    with tempfile.TemporaryDirectory() as temporary:
        work = pathlib.Path(temporary) / "work"
        shutil.copytree(folder, work)  # links are copied as what they lead to
        overlay(spec, work)
        for path in mo_files(spec):
            relative = path.relative_to(spec).as_posix()
            status, failed = test_results(work, mo, relative, module_name(path))
            if status != 0 or failed:
                problems.append(f"{relative}: " + ("; ".join(failed) or
                                                   f"mo test exited {status}"))
    return problems


def run_problems(case, mo, main, folder):
    problems = []
    with tempfile.TemporaryDirectory() as temporary:
        work = pathlib.Path(temporary) / "work"
        if (case / "input").exists():
            shutil.copytree(case / "input", work, symlinks=True)
        else:
            work.mkdir()
        program = (folder / main).resolve()
        args = (case / "args").read_text().splitlines() if (case / "args").exists() else []
        stdin = (case / "stdin").read_bytes() if (case / "stdin").exists() else b""
        result = run([mo, "run", str(program), *args], work, stdin)
        if result is None:
            return [f"did not finish within {SECONDS} seconds"]
        expected = (case / "stdout").read_bytes()
        if result.stdout != expected:
            problems.append(f"stdout\n--- expected\n{expected.decode()}--- actual\n"
                            f"{result.stdout.decode(errors='replace')}")
        status = int((case / "status").read_text())
        if result.returncode != status:
            problems.append(f"status: expected {status}, got {result.returncode}")
        if (case / "stderr").exists() and result.stderr != (case / "stderr").read_bytes():
            problems.append(f"stderr\n--- expected\n{(case / 'stderr').read_text()}--- actual\n"
                            f"{result.stderr.decode(errors='replace')}")
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


def grade(task, folder, mo, grammar=None, quiet=False):
    """Returns ("pass" | "fail" | "gap", step, lines)."""
    main = meta(task)["main"]
    lines = []
    if grammar:
        problems = parses(folder, grammar)
        if problems:
            return "fail", 1, problems
    if not (folder / main).exists():
        return "fail", 2, [f"{main} is missing"]
    found = diagnostics(folder, mo, main)
    verdict = "pass"
    if found:
        lines += [text for _, text in found]
        if not all(code in GAP_CODES for code, _ in found):
            return "fail", 2, lines
        verdict = "gap"
    problems = spec_problems(task, folder, mo)
    runs = here / task / "hidden/runs"
    for case in sorted(p for p in runs.iterdir() if p.is_dir()) if runs.exists() else []:
        for problem in run_problems(case, mo, main, folder):
            problems.append(f"run {case.name}: {problem}")
    if problems:
        return ("gap" if verdict == "gap" else "fail"), 3, lines + problems
    return verdict, None, lines


def describe(verdict, step):
    if verdict == "pass":
        return "pass"
    if verdict == "gap":
        return ("toolchain gap if the reference requires each E1103 or E1150 form"
                + (", and hidden tests fail" if step else ""))
    return f"fail at step {step} ({['parses', 'checks', 'does the task'][step - 1]})"


def sums(task):
    folder = here / task
    files = sorted(p for p in folder.rglob("*") if p.is_file() and p.name != "SHA256SUMS"
                   and "__pycache__" not in p.parts)
    return "".join(f"{hashlib.sha256(p.read_bytes()).hexdigest()}  "
                   f"{p.relative_to(folder).as_posix()}\n" for p in files)


def check_task(task, mo):
    """Returns the problems with one task, as lines."""
    problems = []
    data = meta(task)
    folder = here / task
    if not (folder / "SHA256SUMS").exists() or (folder / "SHA256SUMS").read_text() != sums(task):
        problems.append("SHA256SUMS does not match its files; frozen tasks don't change "
                        "(see bench/v0/README.md)")
    verdict, step, lines = grade(task, folder / "hidden/reference", mo)
    if verdict != "pass":
        problems.append(f"the reference: {describe(verdict, step)}")
        problems += ["  " + line for line in lines]
    verdict, step, _ = grade(task, folder / "hidden/broken", mo)
    if verdict != "fail" or step != 3:
        problems.append("the broken solution must check and fail a hidden test")
    if data["kind"] == "write":
        if (folder / "start").exists():
            problems.append("a write task has no start folder")
    else:
        verdict, step, _ = grade(task, folder / "start", mo)
        if verdict != "fail":
            problems.append("the start folder passes; the task would already be done")
        elif data["kind"] == "fix" and step != 2:
            problems.append("the start folder of a fix task must fail mo check")
        elif data["kind"] != "fix" and step != 3:
            problems.append(f"the start folder of a {data['kind']} task must check and "
                            f"fail a hidden test")
    if data["kind"] == "failing-test":
        name = data.get("failing_test", "")
        module = name.split(".test.")[0]
        files = [p for p in mo_files(folder / "start") if module_name(p) == module]
        _, failed = (test_results(folder / "start", mo, files[0].name, module) if files
                     else (None, ["no test module"]))
        if failed != [f"{name} failed"]:
            problems.append(f"in start/, exactly {name} must fail; got {failed}")
    return problems


def main():
    args = sys.argv[1:]
    mo, grammar = "mo", None
    for flag in ("--mo", "--grammar"):
        if flag in args:
            at = args.index(flag)
            if at + 1 >= len(args):
                return usage()
            value = args[at + 1]
            if "/" in value:
                value = str(pathlib.Path(value).resolve())
            mo, grammar = (value, grammar) if flag == "--mo" else (mo, value)
            del args[at:at + 2]
    if args and args[0] == "--check":
        names = args[1:] or tasks()
        if any(name not in tasks() for name in names):
            return usage()
        with concurrent.futures.ThreadPoolExecutor() as pool:
            results = list(pool.map(lambda name: check_task(name, mo), names))
        bad = 0
        for name, problems in zip(names, results):
            print(f"{'ok  ' if not problems else 'FAIL'} {name}")
            for problem in problems:
                print("     " + problem.replace("\n", "\n     "))
            bad += bool(problems)
        print(f"benchmark v0: {len(names) - bad} of {len(names)} tasks frozen and sound")
        return 0 if bad == 0 else 1
    if len(args) == 2 and args[0] == "--freeze" and args[1] in tasks():
        (here / args[1] / "SHA256SUMS").write_text(sums(args[1]))
        return 0
    if len(args) == 2 and args[0] in tasks():
        folder = (here / args[0] / "hidden/reference" if args[1] == "--reference"
                  else pathlib.Path(args[1]).resolve())
        verdict, step, lines = grade(args[0], folder, mo, grammar)
        for line in lines:
            print("  " + line.replace("\n", "\n  "))
        print(f"{args[0]}: {describe(verdict, step)}")
        return 0 if verdict == "pass" else 1
    return usage()


def usage():
    print(__doc__.split("\n\n")[1], file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
