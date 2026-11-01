#!/usr/bin/env python3

import os
import pathlib
import shutil
import subprocess
import tempfile
import threading
import time
import unittest


ROOT = pathlib.Path(__file__).resolve().parent.parent
WAITER = ROOT / "ci/wait-for-results"
PASS = "check.sh finished: pass"
FAIL = "check.sh finished: fail"
LIMIT_SECONDS = 60

SUCCEED = "#!/bin/sh\nexit 0\n"
FAILING = "#!/bin/sh\nexit 1\n"
BLOCK_WHILE_HOLD_EXISTS = '''#!/bin/sh
: >"$BLOCKED"
while [ -e "$HOLD" ]; do sleep 0.02; done
'''
RECORDING_SLEEP = '''#!/bin/sh
echo "$1" >>"$SLEEPS"
if [ "$(wc -l <"$SLEEPS")" -eq "${FINISH_ON_SLEEP:-0}" ]; then
    echo "check.sh finished: pass" >>"$RESULTS"
fi
'''


TAIL_THEN_END_RUN = '''#!/bin/sh
"$REAL_TAIL" "$@"
echo "check.sh finished: pass" >>"$RESULTS"
kill "$RUN"
'''


class WaitForResults(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="mo-wait-for-results-")
        self.addCleanup(temporary.cleanup)
        self.root = pathlib.Path(temporary.name)
        (self.root / "ci").mkdir()
        shutil.copyfile(ROOT / "ci/check.sh", self.root / "ci/check.sh")
        self.commands = self.root / "commands"
        self.commands.mkdir()
        for name in ("make", "gcc", "clang"):
            self.fake(self.commands, name, SUCCEED)
        self.results = self.root / "results.txt"

    def fake(self, directory, name, text):
        path = directory / name
        path.write_text(text)
        path.chmod(0o755)

    def check_environment(self, **settings):
        environment = dict(os.environ, PATH=f"{self.commands}:{os.environ['PATH']}")
        environment.pop("CI", None)
        environment.pop("CC", None)
        environment.update(settings)
        return environment

    def start(self, *groups, **settings):
        with self.results.open("a") as output:
            return subprocess.Popen(["bash", "ci/check.sh", *groups], cwd=self.root,
                                    env=self.check_environment(**settings),
                                    stdout=output, stderr=subprocess.STDOUT)

    def check(self, *groups, **settings):
        return self.start(*groups, **settings).wait(timeout=LIMIT_SECONDS)

    def start_blocked(self):
        self.hold = self.root / "hold"
        blocked = self.root / "blocked"
        self.hold.touch()
        self.fake(self.commands, "make", BLOCK_WHILE_HOLD_EXISTS)
        process = self.start("gcc", HOLD=str(self.hold), BLOCKED=str(blocked))
        self.addCleanup(process.wait, timeout=LIMIT_SECONDS)
        self.addCleanup(self.hold.unlink, missing_ok=True)
        deadline = time.monotonic() + LIMIT_SECONDS
        while not blocked.exists():
            self.assertLess(time.monotonic(), deadline, "check.sh never reached make")
            time.sleep(0.01)
        return process

    def wait(self, *arguments, sleep_settings=None, extra_path=None):
        environment = dict(os.environ)
        if sleep_settings is not None:
            sleeps = self.root / "sleep"
            sleeps.mkdir(exist_ok=True)
            self.fake(sleeps, "sleep", RECORDING_SLEEP)
            environment.update(sleep_settings, PATH=f"{sleeps}:{os.environ['PATH']}",
                               RESULTS=str(self.results))
        if extra_path is not None:
            environment["PATH"] = f"{extra_path}:{environment['PATH']}"
        return subprocess.run([WAITER, *arguments], env=environment, capture_output=True,
                              text=True, timeout=LIMIT_SECONDS)

    def assert_waited(self, status, *arguments):
        result = self.wait(str(self.results), *arguments)
        self.assertEqual(result.returncode, status, result.stdout + result.stderr)
        return result.stdout

    def last_line(self):
        return self.results.read_text().splitlines()[-1]

    def test_pass(self):
        self.assertEqual(self.check("gcc"), 0)
        self.assertEqual(self.last_line(), PASS)
        self.assertNotIn("skipped", self.assert_waited(0))

    def test_pass_with_a_skip_names_what_was_skipped(self):
        self.assertEqual(self.check("sanitize", CC="mo-no-such-compiler"), 0)
        self.assertEqual(self.last_line(), PASS)
        self.assertIn("mo-no-such-compiler", self.assert_waited(0))

    def test_failure_in_make(self):
        self.fake(self.commands, "make", FAILING)
        self.assertEqual(self.check("gcc"), 1)
        self.assertEqual(self.last_line(), FAIL)
        self.assertIn("failed group: gcc", self.assert_waited(1))

    def test_failure_names_the_group_in_force_not_the_last_one_asked_for(self):
        self.fake(self.commands, "make", FAILING)
        self.assertEqual(self.check("gcc", "clang"), 1)
        self.assertIn("failed group: gcc", self.assert_waited(1))

    def test_missing_tool_under_ci(self):
        self.assertEqual(self.check("sanitize", CI="1", CC="mo-no-such-compiler"), 1)
        self.assertEqual(self.last_line(), FAIL)
        self.assertIn("failed group: sanitize", self.assert_waited(1))

    def test_failure_of_a_check_that_ran_later(self):
        self.fake(self.commands, "python3", FAILING)
        self.assertEqual(self.check("drivers"), 1)
        self.assertEqual(self.last_line(), FAIL)
        printed = self.assert_waited(1)
        self.assertIn("failed group: drivers", printed)
        self.assertRegex(printed, "(?m)^failed: .")

    def test_running_then_finished_through_the_documented_loop(self):
        process = self.start_blocked()
        self.assertIn("still running", self.assert_waited(75, "0"))
        self.hold.unlink()
        deadline = time.monotonic() + LIMIT_SECONDS
        status = 75
        while status == 75:
            self.assertLess(time.monotonic(), deadline, "the run never finished")
            status = self.wait(str(self.results), "1").returncode
        self.assertEqual(status, 0)
        self.assertEqual(process.wait(timeout=LIMIT_SECONDS), 0)

    def test_sigterm_is_a_failure(self):
        process = self.start_blocked()
        process.terminate()
        self.assertNotEqual(process.wait(timeout=LIMIT_SECONDS), 0)
        self.assertEqual(self.last_line(), FAIL)
        self.assert_waited(1, "0")

    def test_sigkill_leaves_no_marker_and_is_a_failure(self):
        self.results.write_text("nohup: ignoring input\n")
        process = self.start_blocked()
        process.kill()
        process.wait(timeout=LIMIT_SECONDS)
        self.assertNotIn(self.last_line(), (PASS, FAIL))
        self.assertIn(str(process.pid), self.assert_waited(1, "0"))

    def test_usage_error_run_has_no_marker_and_is_a_failure(self):
        self.assertEqual(self.check("nosuchgroup"), 64)
        self.assertIn("usage:", self.assert_waited(1))

    def test_empty_file_is_running(self):
        self.results.touch()
        self.assert_waited(75, "0")

    def test_a_file_with_no_started_line_is_running_for_its_first_minute(self):
        self.results.write_text("bash: ci/check.sh: No such file or directory\n")
        self.assert_waited(75, "0")

    def test_a_file_with_no_started_line_after_a_minute_is_a_failure(self):
        for text in ("", "bash: ci/check.sh: No such file or directory\n"):
            with self.subTest(text=text):
                self.results.write_text(text)
                old = time.time() - 120
                os.utime(self.results, (old, old))
                self.assert_waited(1, "0")

    def test_a_run_ending_between_the_two_reads_is_not_called_dead(self):
        run = subprocess.Popen(["sleep", "60"])
        self.addCleanup(run.kill)
        reaper = threading.Thread(target=run.wait)
        reaper.start()
        self.addCleanup(reaper.join)
        self.results.write_text(f"check.sh started: pid {run.pid}\noutput\n")
        directory = self.root / "tail"
        directory.mkdir()
        self.fake(directory, "tail", TAIL_THEN_END_RUN)
        sleeps = self.root / "sleeps"
        settings = {"SLEEPS": str(sleeps), "RUN": str(run.pid),
                    "REAL_TAIL": shutil.which("tail")}
        result = self.wait(str(self.results), sleep_settings=settings, extra_path=directory)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_missing_or_irregular_file(self):
        for path in (self.results, self.root):
            with self.subTest(path=path):
                result = self.wait(str(path), "0")
                self.assertEqual(result.returncode, 66)
                self.assertIn("check.sh", result.stderr)

    def test_bad_arguments(self):
        self.results.touch()
        for arguments in ([], [str(self.results), "1", "2"], [str(self.results), "abc"],
                          [str(self.results), ""]):
            with self.subTest(arguments=arguments):
                self.assertEqual(self.wait(*arguments).returncode, 64)

    def live_results(self, *lines):
        self.results.write_text("\n".join((f"check.sh started: pid {os.getpid()}", *lines)) + "\n")

    def sleeps(self):
        return (self.root / "sleeps").read_text().split()

    def test_default_timeout_and_poll_interval(self):
        self.live_results("output")
        settings = {"SLEEPS": str(self.root / "sleeps")}
        result = self.wait(str(self.results), sleep_settings=settings)
        self.assertEqual(result.returncode, 75)
        self.assertEqual(self.sleeps(), ["15"] * 36)

    def test_timeouts_are_decimal_and_the_last_nap_is_cut_short(self):
        for timeout, expected in (("09", ["9"]), ("020", ["15", "5"])):
            with self.subTest(timeout=timeout):
                self.live_results("output")
                sleeps = self.root / "sleeps"
                sleeps.unlink(missing_ok=True)
                result = self.wait(str(self.results), timeout,
                                   sleep_settings={"SLEEPS": str(sleeps)})
                self.assertEqual(result.returncode, 75)
                self.assertEqual(self.sleeps(), expected)

    def test_polling_sees_a_finish(self):
        self.live_results("output")
        settings = {"SLEEPS": str(self.root / "sleeps"), "FINISH_ON_SLEEP": "2"}
        result = self.wait(str(self.results), sleep_settings=settings)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(len(self.sleeps()), 2)

    def test_a_marker_before_the_last_line_does_not_finish_a_run(self):
        for marker in (PASS, FAIL):
            with self.subTest(marker=marker):
                self.live_results(marker, "more output")
                self.assert_waited(75, "0")


if __name__ == "__main__":
    unittest.main()
