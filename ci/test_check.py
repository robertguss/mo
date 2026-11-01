#!/usr/bin/env python3

import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parent.parent
COMMAND = r'''
import json
import os
import pathlib
import sys
import time

name = pathlib.Path(sys.argv[0]).name
if name == "make":
    sys.exit(0)
if name == "getconf":
    print(4)
    sys.exit(0)
args = sys.argv[1:]
if name == "python3":
    name = pathlib.Path(args.pop(0)).name
hidden = name == "check_tools.py" and "--no-hidden" not in args
timed = name == "check_check_time.py"
kind = "hidden" if hidden else "timing" if timed else "parallel"
record = {"pid": os.getpid(), "name": name, "args": args, "kind": kind}
def event(phase):
    with open(os.environ["EVENTS"], "a") as output:
        output.write(json.dumps(dict(record, phase=phase)) + "\n")
event("begin")
time.sleep(0.2 if kind != "parallel" else 0.1)
event("end")
fail = os.environ.get("FAIL", "")
sys.exit(1 if fail == kind and (kind != "parallel" or name in ("check_cli.py", "grade.py")) else 0)
'''


THREE = ["gcc/debug", "clang/debug", "clang/sanitize"]


class Scheduling(unittest.TestCase):
    """The drivers and bench groups: which built executables they drive, and that the hidden
    tool tests and the timed check run alone, after the checks that ran side by side."""

    def run_group(self, group, builds, failure="", all_drivers=True):
        with tempfile.TemporaryDirectory(prefix="mo-check-scheduling-") as temporary:
            root = pathlib.Path(temporary)
            (root / "ci").mkdir()
            shutil.copyfile(ROOT / "ci/check.sh", root / "ci/check.sh")
            commands = root / "commands"
            commands.mkdir()
            for path in [commands / "make", commands / "getconf", commands / "python3",
                         commands / "gcc", commands / "clang",
                         root / "build/gcc/debug/tests/test_format.test",
                         *[root / "build" / build / "mo" for build in builds],
                         *[root / "build/gcc/debug/tests" / name for name in
                           ("test_bytecode_text.test", "test_codegen.test", "test_unicode16.test")]]:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(f"#!{sys.executable}\n" + COMMAND)
                path.chmod(0o755)
            events = root / "events"
            environment = dict(os.environ, PATH=f"{commands}:{os.environ['PATH']}",
                               CI="1", EVENTS=str(events), FAIL=failure)
            environment.pop("MO_ALL_DRIVERS", None)
            if all_drivers:
                environment["MO_ALL_DRIVERS"] = "1"
            result = subprocess.run(["bash", "ci/check.sh", group], cwd=root,
                                    env=environment, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 1 if failure else 0,
                             result.stdout + result.stderr)
            records = [json.loads(line) for line in events.read_text().splitlines()]
            active = {}
            hidden = []
            ordinary_tools = []
            timing = []
            driven = set()
            for record in records:
                if record["phase"] == "end":
                    del active[record["pid"]]
                    continue
                if record["kind"] in ("hidden", "timing"):
                    self.assertFalse(active, f"{record['kind']} overlapped {active}")
                self.assertTrue(all(kind == "parallel" for kind in active.values()),
                                f"{record['name']} started during {active}")
                active[record["pid"]] = record["kind"]
                driven.update(arg for arg in record["args"] if arg.endswith("/mo"))
                if record["kind"] == "hidden":
                    hidden.append(record["args"])
                elif record["kind"] == "timing":
                    timing.append(record["args"])
                elif record["name"] == "check_tools.py":
                    ordinary_tools.append(record["args"])
            self.assertFalse(active)
            self.assertTrue(result.stdout.rstrip().endswith(
                "check.sh finished: fail" if failure else "check.sh finished: pass"), result.stdout)
            return hidden, sorted(ordinary_tools), timing, driven

    def test_drivers_drives_every_build_and_times_the_first(self):
        hidden, tools, timing, driven = self.run_group("drivers", THREE)
        self.assertEqual((hidden, tools, timing), ([], [], [["build/gcc/debug/mo"]]))
        self.assertEqual(driven, {f"build/{build}/mo" for build in THREE})

    def test_bench_runs_the_hidden_tests_on_the_first_build_alone(self):
        hidden, tools, timing, driven = self.run_group("bench", THREE)
        self.assertEqual(hidden, [["build/gcc/debug/mo"]])
        self.assertEqual(tools, sorted([f"build/{build}/mo", "--no-hidden"] for build in THREE[1:]))
        self.assertEqual(timing, [])
        self.assertEqual(driven, {f"build/{build}/mo" for build in THREE})

    def test_one_build(self):
        for build in ("gcc/debug", "clang/debug"):
            with self.subTest(build=build):
                self.assertEqual(self.run_group("drivers", [build]),
                                 ([], [], [[f"build/{build}/mo"]], {f"build/{build}/mo"}))
                self.assertEqual(self.run_group("bench", [build]),
                                 ([[f"build/{build}/mo"]], [], [], {f"build/{build}/mo"}))

    def test_the_first_build_alone_unless_asked_for_all(self):
        for group, expected in (("drivers", ([], [], [["build/gcc/debug/mo"]])),
                                ("bench", ([["build/gcc/debug/mo"]], [], []))):
            with self.subTest(group=group):
                hidden, tools, timing, driven = self.run_group(group, THREE, all_drivers=False)
                self.assertEqual((hidden, tools, timing), expected)
                self.assertEqual(driven, {"build/gcc/debug/mo"})

    def test_failures_still_run_isolated_checks(self):
        for group, failure in (("drivers", "parallel"), ("drivers", "timing"),
                               ("bench", "parallel"), ("bench", "hidden")):
            with self.subTest(group=group, failure=failure):
                hidden, _, timing, _ = self.run_group(group, THREE, failure)
                self.assertEqual((hidden, timing), ([], [["build/gcc/debug/mo"]]) if group == "drivers"
                                 else ([["build/gcc/debug/mo"]], []))


class SanitizeCompiler(unittest.TestCase):
    def run_group(self, compiler):
        with tempfile.TemporaryDirectory(prefix="mo-check-sanitize-") as temporary:
            root = pathlib.Path(temporary)
            (root / "ci").mkdir()
            shutil.copyfile(ROOT / "ci/check.sh", root / "ci/check.sh")
            commands = root / "commands"
            commands.mkdir()
            calls = root / "calls"
            calls.touch()
            for name in ("make", "gcc", "clang"):
                (commands / name).write_text('#!/bin/sh\necho "$*" >>"$CALLS"\n')
                (commands / name).chmod(0o755)
            environment = dict(os.environ, PATH=f"{commands}:{os.environ['PATH']}", CI="1",
                               CALLS=str(calls))
            environment.pop("CC", None)
            if compiler is not None:
                environment["CC"] = compiler
            result = subprocess.run(["bash", "ci/check.sh", "sanitize"], cwd=root,
                                    env=environment, capture_output=True, text=True, timeout=30)
            return result.returncode, calls.read_text().splitlines()

    def test_clang_is_the_default(self):
        for compiler in (None, ""):
            with self.subTest(compiler=compiler):
                self.assertEqual(self.run_group(compiler),
                                 (0, ["--no-print-directory sanitize CC=clang"]))

    def test_cc_chooses_the_compiler(self):
        for compiler in ("gcc", "clang", "gcc -m64"):
            with self.subTest(compiler=compiler):
                self.assertEqual(self.run_group(compiler),
                                 (0, [f"--no-print-directory sanitize CC={compiler}"]))

    def test_a_missing_cc_fails_instead_of_using_another_compiler(self):
        self.assertEqual(self.run_group("mo-no-such-compiler"), (1, []))


if __name__ == "__main__":
    unittest.main()
