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
if name == "cargo":
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
time.sleep(0.15 if kind != "parallel" else 0.05)
event("end")
fail = os.environ.get("FAIL", "")
sys.exit(1 if fail == kind and (kind != "parallel" or name in ("check_cli.py", "grade.py")) else 0)
'''


class Scheduling(unittest.TestCase):
    """The drivers and bench groups: the built pure-Rust mo executable is driven, and the hidden
    tool tests and the timed check run alone, after the checks that ran side by side."""

    def run_group(self, group, failure="", env=None):
        with tempfile.TemporaryDirectory(prefix="mo-check-scheduling-") as temporary:
            root = pathlib.Path(temporary)
            (root / "ci").mkdir()
            shutil.copyfile(ROOT / "ci/check.sh", root / "ci/check.sh")
            commands = root / "commands"
            commands.mkdir()
            for path in [
                commands / "cargo",
                commands / "getconf",
                commands / "python3",
                root / "target/release/mo",
            ]:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(f"#!{sys.executable}\n" + COMMAND)
                path.chmod(0o755)
            events = root / "events"
            environment = dict(
                os.environ,
                PATH=f"{commands}:{os.environ['PATH']}",
                CI="1",
                EVENTS=str(events),
                FAIL=failure,
            )
            for name in ("MO_BIN", "MO_SHARED_CHECKS", "MO_SIDE_BY_SIDE"):
                environment.pop(name, None)
            environment.update(env or {})
            result = subprocess.run(
                ["bash", "ci/check.sh", group],
                cwd=root,
                env=environment,
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(
                result.returncode,
                1 if failure else 0,
                result.stdout + result.stderr,
            )
            records = [json.loads(line) for line in events.read_text().splitlines()]
            active = {}
            hidden = []
            timing = []
            driven = set()
            for record in records:
                if record["phase"] == "end":
                    del active[record["pid"]]
                    continue
                if record["kind"] in ("hidden", "timing"):
                    self.assertFalse(active, f"{record['kind']} overlapped {active}")
                self.assertTrue(
                    all(kind == "parallel" for kind in active.values()),
                    f"{record['name']} started during {active}",
                )
                active[record["pid"]] = record["kind"]
                driven.update(arg for arg in record["args"] if arg.endswith("/mo"))
                if record["kind"] == "hidden":
                    hidden.append(record["args"])
                elif record["kind"] == "timing":
                    timing.append(record["args"])
            self.assertFalse(active)
            self.assertTrue(
                result.stdout.rstrip().endswith(
                    "check.sh finished: fail" if failure else "check.sh finished: pass"
                ),
                result.stdout,
            )
            return hidden, timing, driven

    def test_drivers_drives_release_mo_and_times_alone(self):
        hidden, timing, driven = self.run_group("drivers")
        self.assertEqual((hidden, timing), ([], [["target/release/mo"]]))
        self.assertEqual(driven, {"target/release/mo"})

    def test_bench_runs_hidden_tool_tests_alone(self):
        hidden, timing, driven = self.run_group("bench")
        self.assertEqual(hidden, [["target/release/mo"]])
        self.assertEqual(timing, [])
        self.assertEqual(driven, {"target/release/mo"})

    def test_failures_still_run_isolated_checks(self):
        for group, failure in (
            ("drivers", "parallel"),
            ("drivers", "timing"),
            ("bench", "parallel"),
            ("bench", "hidden"),
        ):
            with self.subTest(group=group, failure=failure):
                hidden, timing, _ = self.run_group(group, failure)
                self.assertEqual(
                    (hidden, timing),
                    ([], [["target/release/mo"]])
                    if group == "drivers"
                    else ([["target/release/mo"]], []),
                )

    def test_unknown_group_exits_64(self):
        result = subprocess.run(
            ["bash", str(ROOT / "ci/check.sh"), "gcc"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=15,
        )
        self.assertEqual(result.returncode, 64)
        self.assertIn("usage: bash ci/check.sh", result.stderr)


class RustGroup(unittest.TestCase):
    def test_rust_runs_fmt_clippy_tests_doc_build_and_audit(self):
        with tempfile.TemporaryDirectory(prefix="mo-check-rust-") as temporary:
            root = pathlib.Path(temporary)
            (root / "ci").mkdir()
            shutil.copyfile(ROOT / "ci/check.sh", root / "ci/check.sh")
            commands = root / "commands"
            commands.mkdir()
            calls = root / "calls"
            calls.touch()
            for name in ("cargo", "python3"):
                (commands / name).write_text(f'#!/bin/sh\necho "{name} $*" >>"$CALLS"\n')
                (commands / name).chmod(0o755)
            environment = dict(
                os.environ,
                PATH=f"{commands}:{os.environ['PATH']}",
                CI="1",
                CALLS=str(calls),
            )
            environment.pop("MO_BIN", None)
            result = subprocess.run(
                ["bash", "ci/check.sh", "rust"],
                cwd=root,
                env=environment,
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue(result.stdout.rstrip().endswith("check.sh finished: pass"))
            self.assertEqual(
                calls.read_text().splitlines(),
                [
                    "cargo fmt --all -- --check",
                    "cargo clippy --workspace --all-targets --all-features -- -D warnings",
                    "cargo test --workspace --all-targets",
                    "cargo test --workspace --doc",
                    "cargo doc --workspace --no-deps",
                    "cargo build --release --bin mo --bin mo-fe --bin mo-rt --bin mo-test-cli --bin mo-test-deadline",
                    "python3 ci/check_rust_frontend.py --audit-only --mo target/release/mo",
                ],
            )

    def test_missing_cargo_respects_ci(self):
        cargo_path = shutil.which("cargo")
        cargo_dir = os.path.realpath(os.path.dirname(cargo_path)) if cargo_path else None
        filtered = [
            entry
            for entry in os.environ.get("PATH", "").split(os.pathsep)
            if entry and (cargo_dir is None or os.path.realpath(entry) != cargo_dir)
        ]
        if shutil.which("cargo", path=os.pathsep.join(filtered)) is not None:
            self.skipTest("cargo shares a system PATH directory with core utilities")
        with tempfile.TemporaryDirectory(prefix="mo-check-rust-nocargo-") as temporary:
            root = pathlib.Path(temporary)
            (root / "ci").mkdir()
            shutil.copyfile(ROOT / "ci/check.sh", root / "ci/check.sh")
            env_base = dict(os.environ, PATH=os.pathsep.join(filtered))
            env_base.pop("CI", None)
            skip_res = subprocess.run(
                ["bash", "ci/check.sh", "rust"],
                cwd=root,
                env=env_base,
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(skip_res.returncode, 0, skip_res.stdout + skip_res.stderr)
            self.assertIn("skipped: cargo is not installed", skip_res.stdout)
            fail_res = subprocess.run(
                ["bash", "ci/check.sh", "rust"],
                cwd=root,
                env=dict(env_base, CI="1"),
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(fail_res.returncode, 1, fail_res.stdout + fail_res.stderr)
            self.assertIn("error: cargo is not installed", fail_res.stderr)


if __name__ == "__main__":
    unittest.main()
