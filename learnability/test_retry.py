#!/usr/bin/env python3
"""Tests for learnability/retry.py. Run: python3 learnability/test_retry.py"""

import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

here = pathlib.Path(__file__).resolve().parent
TALLY_TESTS = here.parent / "bench/tools/tally/hidden/tests"

# A program whose functions start where the diagnostics below point: f at line 1, g at 4, h at 7.
SOURCE = "fn f() -> Int\n  1\n\nfn g() -> Int\n  2\n\nfn h() -> Int\n  3\n"
LINES = {"f": 2, "g": 5, "h": 8}


def run(*args):
    return subprocess.run([sys.executable, str(here / "retry.py"), *map(str, args)],
                          capture_output=True, text=True, check=False, timeout=120)


def write_session(folder, runs):
    """Writes a session log: each run is (command, [(code, function)], totals or None)."""
    folder.mkdir(parents=True)
    for number, (command, errors, totals) in enumerate(runs, 1):
        run_folder = folder / f"{number:03}"
        (run_folder / "src").mkdir(parents=True)
        (run_folder / "src/main.mo").write_text(SOURCE)
        (run_folder / "command").write_text(command + "\n")
        if command == "fix":
            continue
        records = [{"record": "version", "format": "1.0", "mo": "0.0.0", "command": command}]
        for code, function in errors:
            records.append({"record": "diagnostic", "code": code,
                            "primary": {"file": "main.mo", "line": LINES[function]}})
        if totals:
            records.append({"record": "totals", "passed": totals[0], "failed": totals[1],
                            "stopped": 0})
        (run_folder / "output.jsonl").write_text(
            "".join(json.dumps(record) + "\n" for record in records))


class Retry(unittest.TestCase):
    def setUp(self):
        self.logs = pathlib.Path(tempfile.mkdtemp()) / "logs"
        self.logs.mkdir()
        self.addCleanup(shutil.rmtree, self.logs.parent)

    def report(self, status=0):
        result = run(self.logs)
        self.assertEqual(result.returncode, status, result.stderr)
        table, _, rest = result.stdout.partition("\n\n")
        everything, _, measured = rest.partition("\n\n")
        return table, everything.split("\n"), measured.rstrip("\n").split("\n")

    def test_kept_codes_left_out(self):
        # E0050 in f is fixed by the next run; E1150 in g takes two. Only the second set drops
        # E1150, and only the second set is measured against the targets.
        write_session(self.logs / "s1", [("check", [("E0050", "f"), ("E1150", "g")], None),
                                         ("check", [("E1150", "g")], None),
                                         ("test", [], (1, 0))])
        _, everything, measured = self.report()
        self.assertEqual(everything, [
            "All errors, as tally counts them:",
            "  errors: 2 started, 0 moved to by_fix by a mo fix run, 2 rated",
            "  first retry: 1/2 = 50.0%, 95% Wilson interval [9.5%, 90.5%]",
            "  within three: 2/2 = 100.0%, 95% Wilson interval [34.2%, 100.0%]",
            "  sessions: 1; the most rated errors, 2 of 2, came from session s1",
            "  without session s1, first retry: no errors to rate",
            "  without session s1, within three: no errors to rate"])
        self.assertEqual(measured, [
            "Without E1103, E1150 and E1400, the codes the guide tells sessions to keep "
            "(measured against the targets):",
            "  errors: 1 started, 0 moved to by_fix by a mo fix run, 1 rated",
            "  first retry: 1/1 = 100.0%, 95% Wilson interval [20.7%, 100.0%]; "
            "target 85.0%: point-estimate pass",
            "  within three: 1/1 = 100.0%, 95% Wilson interval [20.7%, 100.0%]; "
            "target 95.0%: point-estimate pass",
            "  sessions: 1; the most rated errors, 1 of 1, came from session s1",
            "  without session s1, first retry: no errors to rate",
            "  without session s1, within three: no errors to rate"])

    def test_every_kept_code(self):
        # E1103, E1150 and E1400 are all left out, and nothing else is. The three errors of the
        # first run are fixed by the second, which ends the session with E1400 unfixed.
        write_session(self.logs / "s1", [
            ("check", [("E1103", "f"), ("E1150", "g"), ("E0078", "h")], None),
            ("test", [("E1400", "f")], None)])
        _, everything, measured = self.report()
        self.assertEqual(everything[1], "  errors: 4 started, 0 moved to by_fix by a mo fix run, "
                         "4 rated")
        self.assertEqual(measured[1], "  errors: 1 started, 0 moved to by_fix by a mo fix run, "
                         "1 rated")
        self.assertEqual(measured[2], "  first retry: 1/1 = 100.0%, 95% Wilson interval "
                         "[20.7%, 100.0%]; target 85.0%: point-estimate pass")

    def test_session_with_the_most_left_out(self):
        # Session a fixes its one error on the first retry. Session b fixes two of its three
        # and ends with E0078 unfixed, so it has the most, and the rates without it are a's.
        write_session(self.logs / "a", [("check", [("E0050", "f")], None), ("check", [], None)])
        write_session(self.logs / "b", [
            ("check", [("E0050", "f"), ("E0078", "g"), ("E0024", "h")], None),
            ("check", [("E0078", "g")], None)])
        _, everything, _ = self.report()
        self.assertEqual(everything[2:], [
            "  first retry: 3/4 = 75.0%, 95% Wilson interval [30.1%, 95.4%]",
            "  within three: 3/4 = 75.0%, 95% Wilson interval [30.1%, 95.4%]",
            "  sessions: 2; the most rated errors, 3 of 4, came from session b",
            "  without session b, first retry: 1/1 = 100.0%, 95% Wilson interval [20.7%, 100.0%]",
            "  without session b, within three: 1/1 = 100.0%, 95% Wilson interval "
            "[20.7%, 100.0%]"])

    def test_tie_leaves_out_the_first(self):
        write_session(self.logs / "a", [("check", [("E0050", "f")], None), ("check", [], None)])
        write_session(self.logs / "b", [("check", [("E0050", "f")], None),
                                        ("check", [("E0050", "f")], None)])
        _, everything, _ = self.report()
        self.assertEqual(everything[4:], [
            "  sessions: 2; the most rated errors, 1 of 2, came from session a, tied with b",
            "  without session a, first retry: 0/1 = 0.0%, 95% Wilson interval [0.0%, 79.3%]",
            "  without session a, within three: 0/1 = 0.0%, 95% Wilson interval [0.0%, 79.3%]"])

    def test_by_fix_is_not_rated(self):
        # A mo fix run between the error's last report and the run without it moves it to by_fix.
        write_session(self.logs / "s1", [("check", [("E0050", "f"), ("E0078", "g")], None),
                                         ("fix", [], None),
                                         ("check", [("E0078", "g")], None),
                                         ("check", [], None)])
        _, everything, _ = self.report()
        self.assertEqual(everything[1:4], [
            "  errors: 2 started, 1 moved to by_fix by a mo fix run, 1 rated",
            "  first retry: 0/1 = 0.0%, 95% Wilson interval [0.0%, 79.3%]",
            "  within three: 1/1 = 100.0%, 95% Wilson interval [20.7%, 100.0%]"])

    def test_target_pass(self):
        # Protocol section 7: 22 errors all fixed on the first retry pass 85%; within three, 95%
        # needs 73, so 22 is only a point-estimate pass.
        for number in range(22):
            write_session(self.logs / f"s{number:02}", [("check", [("E0050", "f")], None),
                                                        ("check", [], None)])
        _, _, measured = self.report()
        self.assertEqual(measured[2:4], [
            "  first retry: 22/22 = 100.0%, 95% Wilson interval [85.1%, 100.0%]; "
            "target 85.0%: pass",
            "  within three: 22/22 = 100.0%, 95% Wilson interval [85.1%, 100.0%]; "
            "target 95.0%: point-estimate pass"])

    def test_no_errors(self):
        write_session(self.logs / "s1", [("check", [], None)])
        (self.logs / "s2").mkdir()
        _, everything, measured = self.report()
        self.assertEqual(everything[1:], [
            "  errors: 0 started, 0 moved to by_fix by a mo fix run, 0 rated",
            "  first retry: no errors to rate",
            "  within three: no errors to rate",
            "  sessions: 2; none has a rated error"])
        self.assertEqual(measured[-1], "  sessions: 2; none has a rated error")

    def test_no_sessions(self):
        _, everything, _ = self.report()
        self.assertEqual(everything[-1], "  sessions: 0")

    def test_table_as_tally_prints_it(self):
        # Every hidden tally test with logs: the table is the test's expected output, word for
        # word, and the first set's rates are the table's.
        cases = [case for case in sorted(TALLY_TESTS.iterdir())
                 if (case / "input/logs").is_dir()]
        self.assertGreaterEqual(len(cases), 14)
        for case in cases:
            with self.subTest(case=case.name):
                result = run(case / "input/logs")
                self.assertEqual(result.returncode, int((case / "status").read_text()),
                                 result.stderr)
                heading, _, rest = result.stdout.partition("\n")
                self.assertEqual(heading, "tally (bench/tools/tally/hidden/reference.py) over "
                                 f"{case / 'input/logs'}:")
                expected = (case / "stdout").read_text()
                self.assertEqual(rest[:len(expected)], expected)
                totals = expected.split("\ntotal\t")[1].split("\n")[0].split("\t")
                errors, first, three, by_fix = (int(t) for t in totals[:4])
                if errors - by_fix:
                    self.assertIn(f"  first retry: {first}/{errors - by_fix} = ", rest)
                    self.assertIn(f"  within three: {three}/{errors - by_fix} = ", rest)

    def test_line_not_json(self):
        # tally skips the run, says so on standard error and exits 1; the numbers still print.
        shutil.rmtree(self.logs)
        shutil.copytree(TALLY_TESTS / "15-bad-lines/input/logs", self.logs)
        result = run(self.logs)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stderr.count("002/output.jsonl:3: not JSON"), 1, result.stderr)
        self.assertIn("\n  first retry: ", result.stdout)

    def test_tally_fails(self):
        # A diagnostic with no file stops the reference with a traceback (harness README), which
        # also exits 1 but prints no table: that is a failure, not a skipped line.
        write_session(self.logs / "s1", [("check", [("E0050", "f")], None)])
        output = self.logs / "s1/001/output.jsonl"
        output.write_text(output.read_text().replace('"file": "main.mo"', '"file": null'))
        result = run(self.logs)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, "")
        self.assertIn("retry.py: the reference tally printed no table", result.stderr)
        self.assertIn("TypeError", result.stderr)

    def test_sessions_must_add_up(self):
        # The leave-one-out rates rely on tally adding up its sessions. A tally that didn't
        # (here, one that finds one error in each session alone but one in all) stops the script.
        fake = self.logs.parent / "fake.py"
        fake.write_text("import os, sys\n"
                        "print('code\\terrors\\tfirst_retry\\twithin_three\\tby_fix\\tunfixed')\n"
                        "print('E0050\\t1\\t1\\t1\\t0\\t0')\n"
                        "print('total\\t1\\t1\\t1\\t0\\t0')\n")
        (self.logs / "a").mkdir()
        (self.logs / "b").mkdir()
        sys.path.insert(0, str(here))
        import retry
        saved = retry.REFERENCE
        retry.REFERENCE = fake
        self.addCleanup(setattr, retry, "REFERENCE", saved)
        with self.assertRaisesRegex(retry.Failed, "the sessions' tallies don't add up to the "
                                    "tally over .* for E0050"):
            retry.main([str(self.logs)])

    def test_usage(self):
        for args in [(), (self.logs, self.logs)]:
            result = run(*args)
            self.assertEqual((result.returncode, result.stdout), (2, ""))
            self.assertEqual(result.stderr, "retry.py: usage: retry.py LOGS\n")
        result = run(self.logs / "nowhere")
        self.assertEqual((result.returncode, result.stdout), (2, ""))
        self.assertEqual(result.stderr, f"retry.py: can't read {self.logs / 'nowhere'}\n")


if __name__ == "__main__":
    unittest.main()
