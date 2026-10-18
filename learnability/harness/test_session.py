#!/usr/bin/env python3
"""Tests for learnability/harness/session.py against a built mo.

Run: python3 learnability/harness/test_session.py build/gcc/debug/mo [more mo binaries]
"""

import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

here = pathlib.Path(__file__).resolve().parent
repo = here.parent.parent
harness = here / "session.py"
example = repo / "bench/tools/tally/hidden/tests/01-example"
tally = repo / "bench/tools/tally/hidden/reference.py"
MO = None  # set per binary by main


def run(*args, cwd=None, path=None):
    env = dict(os.environ)
    if path is not None:
        env["PATH"] = f"{path}:{env['PATH']}"
    return subprocess.run([str(a) for a in args], cwd=cwd, env=env, capture_output=True,
                          check=False, timeout=120)


def untimed(jsonl):
    """The records with their timings left out, since two runs of a test take different times."""
    records = [json.loads(line) for line in jsonl.splitlines()]
    for record in records:
        record.pop("microseconds", None)
    return records


def digest(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


class Harness(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)

    def start(self, name="s1"):
        session = self.tmp / name
        got = run(sys.executable, harness, "start", "--mo", MO, "--sha256", digest(MO), session)
        self.assertEqual(got.returncode, 0, got.stderr)
        return session

    def mo(self, session, *args, cwd=None):
        """Runs `mo ARGS` as a session would, through PATH, and the real mo the same way."""
        cwd = cwd or session / "work"
        seen = run("mo", *args, cwd=cwd, path=session / "bin")
        real = run(MO, *args, cwd=cwd)
        self.assertEqual((seen.stdout, seen.stderr, seen.returncode),
                         (real.stdout, real.stderr, real.returncode), args)
        return seen

    def source(self, session, run_number):
        shutil.copyfile(example / f"input/logs/s1/{run_number}/src/main.mo",
                        session / "work/main.mo")

    def test_replays_the_tally_example(self):
        # The example of bench/tools/tally/task.md, run for real: the session's log reads to the
        # example's table, and commands that check nothing stay out of it.
        session = self.start()
        work = session / "work"
        self.source(session, "001")
        self.mo(session, "check", "main.mo")
        self.mo(session, "check")  # refused: names no file
        self.mo(session, "fix", "--json", "main.mo")  # refused: a flag fix doesn't take
        ran = self.mo(session, "run", "main.mo")
        self.source(session, "002")
        (work / "answers").mkdir()
        self.mo(session, "check", "--json", "../main.mo", cwd=work / "answers")
        self.source(session, "003")
        self.mo(session, "test", str(work / "main.mo"))
        self.mo(session, "guide")

        log = session / "log"
        self.assertEqual(sorted(os.listdir(log)), ["001", "002", "003"])
        self.assertEqual(run(sys.executable, harness, "check-log", log).returncode, 0)
        for number, command in [("001", "check"), ("002", "check"), ("003", "test")]:
            folder = log / number
            self.assertEqual((folder / "command").read_text(), command + "\n")
            self.assertEqual(sorted(os.listdir(folder)), ["command", "output.jsonl", "src"])
            self.assertEqual((folder / "src/main.mo").read_bytes(),
                             (example / f"input/logs/s1/{number}/src/main.mo").read_bytes())
            # The JSON is mo's own, as typed in the work folder, whatever the session typed.
            self.source(session, number)
            want = run(MO, command, "--json", "main.mo", cwd=work).stdout
            got = (folder / "output.jsonl").read_bytes()
            self.assertEqual(untimed(got), untimed(want))
            for line in got.splitlines():
                record = json.loads(line)
                for span in [record.get("primary"), record.get("span")]:
                    if span:
                        self.assertEqual(span["file"], "main.mo")

        logs = self.tmp / "logs"
        self.assertEqual(run(sys.executable, harness, "collect", logs, session).returncode, 0)
        got = run(sys.executable, tally, "logs", cwd=self.tmp)
        self.assertEqual(got.returncode, 0, got.stderr)
        self.assertEqual(got.stdout, (example / "stdout").read_bytes())

        runs = session / "runs"
        self.assertEqual(len(os.listdir(runs)), 7)
        argv = [json.loads((runs / n / "argv").read_text()) for n in sorted(os.listdir(runs))]
        self.assertEqual(argv[0], ["check", "main.mo"])
        self.assertEqual(argv[3], ["run", "main.mo"])
        self.assertEqual([(runs / n / "log").read_text() if (runs / n / "log").exists() else None
                          for n in sorted(os.listdir(runs))],
                         ["001\n", None, None, None, "002\n", "003\n", None])
        self.assertEqual((runs / "005/cwd").read_text(), "answers\n")
        self.assertEqual((runs / "002/status").read_text(), "2\n")
        self.assertEqual(((runs / "004/stdout").read_bytes(), (runs / "004/stderr").read_bytes()),
                         (ran.stdout, ran.stderr))
        self.assertEqual(os.listdir(session / ".pending"), [])

    def test_fix_runs_are_logged(self):
        session = self.start()
        self.source(session, "001")
        self.mo(session, "fix", "main.mo")
        self.assertEqual((session / "log/001/command").read_text(), "fix\n")
        self.assertEqual(sorted(os.listdir(session / "log/001")), ["command", "src"])

    def test_concurrent_runs_get_distinct_numbers(self):
        session = self.start()
        self.source(session, "001")
        env = dict(os.environ, PATH=f"{session / 'bin'}:{os.environ['PATH']}")
        procs = [subprocess.Popen(["mo", "check", "main.mo"], cwd=session / "work", env=env,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                 for _ in range(8)]
        for proc in procs:
            self.assertEqual(proc.wait(timeout=120), 1)
        self.assertEqual(sorted(os.listdir(session / "log")), [f"{n:03d}" for n in range(1, 9)])
        self.assertEqual(run(sys.executable, harness, "check-log", session / "log").returncode, 0)

    def test_start_refuses_another_binary(self):
        got = run(sys.executable, harness, "start", "--mo", MO, "--sha256", "0" * 64,
                  self.tmp / "s")
        self.assertEqual(got.returncode, 1)
        self.assertIn(b"not " + b"0" * 64, got.stderr)
        self.assertFalse((self.tmp / "s").exists())

    def test_start_refuses_a_used_folder(self):
        self.start("s")
        got = run(sys.executable, harness, "start", "--mo", MO, "--sha256", digest(MO),
                  self.tmp / "s")
        self.assertEqual(got.returncode, 1)
        self.assertIn(b"is not empty", got.stderr)

    def test_check_log_reports_a_bad_log(self):
        log = self.tmp / "log"
        (log / "001/src").mkdir(parents=True)
        (log / "001/command").write_text("run\n")
        (log / "003/src").mkdir(parents=True)
        (log / "003/command").write_text("check\n")
        (log / "003/output.jsonl").write_text('{"record":"totals"}\n')
        (log / "notes.md").write_text("")
        got = run(sys.executable, harness, "check-log", log)
        self.assertEqual(got.returncode, 1)
        for problem in [b"notes.md: not a run's folder", b"not numbered 001 to 002",
                        b"001: command is 'run\\n'", b"003: output.jsonl doesn't start"]:
            self.assertIn(problem, got.stderr)


def main():
    binaries = sys.argv[1:]
    if not binaries:
        print(__doc__.split("\n\n")[1], file=sys.stderr)
        return 2
    global MO
    ok = True
    for binary in binaries:
        MO = os.path.abspath(binary)
        print(f"session harness with {binary}", file=sys.stderr)
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(Harness)
        ok = unittest.TextTestRunner(verbosity=1).run(suite).wasSuccessful() and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
