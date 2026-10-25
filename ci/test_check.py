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
sys.exit(1 if fail == kind and (kind != "parallel" or name == "check_cli.py") else 0)
'''


class Scheduling(unittest.TestCase):
    def run_driver(self, builds, failure=""):
        with tempfile.TemporaryDirectory(prefix="mo-check-scheduling-") as temporary:
            root = pathlib.Path(temporary)
            (root / "ci").mkdir()
            shutil.copyfile(ROOT / "ci/check.sh", root / "ci/check.sh")
            commands = root / "commands"
            commands.mkdir()
            for path in [commands / "make", commands / "getconf", commands / "python3",
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
            result = subprocess.run(["bash", "ci/check.sh", "drivers"], cwd=root,
                                    env=environment, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 1 if failure else 0,
                             result.stdout + result.stderr)
            records = [json.loads(line) for line in events.read_text().splitlines()]
            active = {}
            hidden = []
            ordinary_tools = []
            timing = []
            for record in records:
                if record["phase"] == "end":
                    del active[record["pid"]]
                    continue
                if record["kind"] in ("hidden", "timing"):
                    self.assertFalse(active, f"{record['kind']} overlapped {active}")
                self.assertTrue(all(kind == "parallel" for kind in active.values()),
                                f"{record['name']} started during {active}")
                active[record["pid"]] = record["kind"]
                if record["kind"] == "hidden":
                    hidden.append(record["args"])
                elif record["kind"] == "timing":
                    timing.append(record["args"])
                elif record["name"] == "check_tools.py":
                    ordinary_tools.append(record["args"])
            self.assertFalse(active)
            self.assertEqual(hidden, [[f"build/{builds[0]}/mo"]])
            self.assertEqual(sorted(ordinary_tools),
                             sorted([[f"build/{build}/mo", "--no-hidden"] for build in builds[1:]]))
            self.assertEqual(timing, [[f"build/{builds[0]}/mo"]])

    def test_three_builds(self):
        self.run_driver(["gcc/debug", "clang/debug", "clang/sanitize"])

    def test_one_build(self):
        for build in ("gcc/debug", "clang/debug"):
            with self.subTest(build=build):
                self.run_driver([build])

    def test_failures_still_run_isolated_checks(self):
        for failure in ("parallel", "hidden", "timing"):
            with self.subTest(failure=failure):
                self.run_driver(["gcc/debug", "clang/debug", "clang/sanitize"], failure)


if __name__ == "__main__":
    unittest.main()
