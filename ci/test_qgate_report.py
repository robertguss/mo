#!/usr/bin/env python3
"""Tests for ci/qgate_report.py's filter and output. Run: python3 ci/test_qgate_report.py

These need no qgate binary: they feed the report saved qgate JSON, shaped like qgate 0.1.0's.
"""

import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

here = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(here))
import qgate_report  # noqa: E402


def finding(rule, path, line=None, message="", severity="error"):
    out = {"rule": rule, "severity": severity, "path": path, "message": message}
    if line is not None:
        out["line"] = line
    return out


LONG = finding("function-length", "src/vm/run.c", 40, "function `step` is 129 lines (limit 50)")
GREW = finding("function-length", "lib/json.mo", 7,
               "function `parse` grew to 90 lines; a baselined violation may not exceed 85")
DUP_IN_SRC = finding("duplication", "src/check/a.c", 3,
                     "duplicated block (60 tokens, lines 3-9) also at src/check/b.c:10-16")
DUP_PARTNER_IN_SRC = finding("duplication", "cbmc/verify.c", 345,
                             "duplicated block (76 tokens, lines 345-361) also at "
                             "src/bytecode/validate.c:520-536; extract the shared code")
DROPPED = [
    finding("function-length", "cbmc/prove.sh", 89, "function `run` is 118 lines"),
    finding("function-length", "ci/gen_core.py", 5, "function `expr` is 51 lines"),
    finding("duplication", "cbmc/verify.c", 55,
            "duplicated block (50 tokens, lines 55-62) also at cbmc/arena.c:116-123"),
    finding("file-length", "src/bytecode/verify.c", 1, "file grew to 717 lines"),
    finding("unapproved-change", "qgate.baseline", None, "baseline: added x in src/a.c = 51"),
    finding("parse-error", "src/vm/run.c", None, "syntax errors", severity="warning"),
    finding("baseline-prunable", "src/vm/run.c", 506, "function shrank", severity="info"),
    finding("function-length", "src/vm/run.c", 9, "function `f` is 60 lines", severity="warning"),
    finding("function-length", "srcx/a.c", 1, "function `g` is 60 lines"),
]


def run_report(data, summary=None):
    with tempfile.TemporaryDirectory() as work:
        saved = pathlib.Path(work) / "run.json"
        saved.write_text(data if isinstance(data, str) else json.dumps(data))
        env = {k: v for k, v in os.environ.items() if k != "GITHUB_STEP_SUMMARY"}
        if summary is not None:
            env["GITHUB_STEP_SUMMARY"] = summary
        return subprocess.run([sys.executable, str(here / "qgate_report.py"), "--findings",
                               str(saved)], capture_output=True, text=True, env=env,
                              check=False, timeout=30)


class Select(unittest.TestCase):
    def test_keeps_new_long_functions_and_duplicates_touching_src_or_lib(self):
        kept = qgate_report.select([*DROPPED, DUP_PARTNER_IN_SRC, GREW, LONG, DUP_IN_SRC])
        self.assertEqual(kept, [DUP_PARTNER_IN_SRC, DUP_IN_SRC, GREW, LONG])

    def test_drops_everything_else(self):
        for f in DROPPED:
            self.assertFalse(qgate_report.keep(f), f)


class Output(unittest.TestCase):
    def test_log_lists_each_finding(self):
        result = run_report({"findings": [LONG, *DROPPED]})
        self.assertEqual(result.returncode, 0)
        lines = result.stdout.splitlines()
        self.assertRegex(lines[0], r"^qgate \(report only, never fails CI\): 1 new finding\(s\)")
        self.assertEqual(lines[1:], ["  function-length: src/vm/run.c:40: "
                                     "function `step` is 129 lines (limit 50)"])

    def test_summary_and_annotations_on_github(self):
        with tempfile.TemporaryDirectory() as work:
            summary = pathlib.Path(work) / "summary.md"
            result = run_report({"findings": [DUP_PARTNER_IN_SRC, *DROPPED]}, str(summary))
            text = summary.read_text()
        self.assertEqual(result.returncode, 0)
        self.assertIn("::notice file=cbmc/verify.c,line=345,title=qgate duplication::"
                      "duplicated block (76 tokens", result.stdout)
        self.assertEqual(result.stdout.count("::notice"), 1)
        self.assertIn("| duplication | `cbmc/verify.c:345` | duplicated block", text)
        self.assertNotIn("file-length", text)

    def test_no_findings(self):
        with tempfile.TemporaryDirectory() as work:
            summary = pathlib.Path(work) / "summary.md"
            result = run_report({"findings": DROPPED}, str(summary))
            text = summary.read_text()
        self.assertIn(": 0 new finding(s)", result.stdout)
        self.assertIn("adds no duplicated code or long functions", text)
        self.assertNotIn("::notice", result.stdout)

    def test_never_fails(self):
        for broken in ["not json", "{}", json.dumps({"findings": [{"rule": "function-length", "severity": "error",
                                                   "path": "src/a.c"}]})]:
            result = run_report(broken)
            self.assertEqual(result.returncode, 0, broken)
            self.assertIn("qgate report skipped:", result.stdout, broken)

    def test_never_fails_without_qgate(self):
        result = subprocess.run([sys.executable, str(here / "qgate_report.py"), "--qgate",
                                 "/nonexistent/qgate", "HEAD"], capture_output=True, text=True,
                                check=False, timeout=60)
        self.assertEqual(result.returncode, 0)
        self.assertIn("qgate report skipped:", result.stdout)


if __name__ == "__main__":
    unittest.main()
