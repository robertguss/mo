#!/usr/bin/env python3
"""Tests for ci/qgate_report.py's filter and output. Run: python3 ci/test_qgate_report.py

These need no qgate binary: they feed the report saved qgate JSON, shaped like qgate 0.1.0's.
"""

import json
import os
import re
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
    finding("function-length", "src/vm/run2.c", 376,
            "baselined function `step` moved here; its baseline entry must move with it"),
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

    def test_annotation_escapes_workflow_commands(self):
        tricky = finding("function-length", "src/a,b:c.c", 2, "100% long\n::error::x")
        self.assertEqual(qgate_report.annotation(tricky),
                         "::notice file=src/a%2Cb%3Ac.c,line=2,title=qgate function-length::"
                         "100%25 long%0A::error::x")
        result = run_report({"findings": [tricky]})
        self.assertNotIn("\n::error", result.stdout)

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
        # Fails before making any worktree, so the real repository is untouched.
        result = subprocess.run([sys.executable, str(here / "qgate_report.py"), "--qgate",
                                 "/nonexistent/qgate", "HEAD"], capture_output=True, text=True,
                                check=False, timeout=60)
        self.assertEqual(result.returncode, 0)
        self.assertIn("qgate report skipped:", result.stdout)


FAKE_QGATE = """#!{python}
import json, pathlib, subprocess, sys
here = pathlib.Path.cwd()
def git(*args):
    return subprocess.run(["git", *args], capture_output=True, text=True, check=True).stdout
if sys.argv[1:] == ["baseline", "create"]:
    (here / "qgate.baseline").write_text("base files: " + " ".join(git("ls-files").split()) + "\\n")
    sys.exit(0)
assert sys.argv[1:] == ["run", "--stage", "commit", "--format", "json"], sys.argv
staged = " ".join(git("diff", "--cached", "--name-status").split())
seen = (here / "qgate.baseline").read_text().strip()
config = (here / "qgate.toml").read_text().splitlines()[-1]
print(json.dumps({{"findings": [
    {{"rule": "function-length", "severity": "error", "path": "src/new.c", "line": 1,
     "message": staged}},
    {{"rule": "function-length", "severity": "error", "path": "src/new.c", "line": 2,
     "message": seen}},
    {{"rule": "function-length", "severity": "error", "path": "src/new.c", "line": 3,
     "message": config}}]}}))
sys.exit(1)
"""


class RunQgate(unittest.TestCase):
    """run_qgate on a scratch repository, with a fake qgate that reports what it was shown."""

    def test_stages_exactly_the_change_on_top_of_a_baseline_of_the_base(self):
        with tempfile.TemporaryDirectory() as work:
            repo = pathlib.Path(work) / "repo"
            (repo / "src").mkdir(parents=True)

            def git(*args):
                subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", *args],
                               cwd=repo, check=True, capture_output=True)

            git("init", "-q")
            (repo / "src/kept.c").write_text("int a;\n")
            (repo / "src/gone.c").write_text("int b;\n")
            git("add", "-A")
            git("commit", "-qm", "base")
            git("branch", "base")
            (repo / "src/kept.c").write_text("int a, c;\n")
            (repo / "src/gone.c").unlink()
            (repo / "src/new.c").write_text("int d;\n")
            git("add", "-A")
            git("commit", "-qm", "change")
            fake = pathlib.Path(work) / "qgate"
            fake.write_text(FAKE_QGATE.format(python=sys.executable))
            fake.chmod(0o755)

            data = qgate_report.run_qgate(str(fake), "base", repo)
            listed = subprocess.run(["git", "worktree", "list", "--porcelain"], cwd=repo,
                                    capture_output=True, text=True, check=True).stdout
        messages = [f["message"] for f in data["findings"]]
        self.assertEqual(messages, [
            "A qgate.baseline A qgate.toml D src/gone.c M src/kept.c A src/new.c",
            "base files: src/gone.c src/kept.c",
            "min_tokens = 50",
        ])
        self.assertEqual(listed.count("worktree "), 1, listed)


class Pins(unittest.TestCase):
    def test_qgate_version_matches_the_workflow(self):
        toml = (here / "qgate.toml").read_text()
        workflow = (here.parent / ".github/workflows/check.yml").read_text()
        version = re.search(r'^qgate_version = "([^"]+)"', toml, re.M).group(1)
        self.assertIn(f"QGATE_VERSION: {version}\n", workflow)


if __name__ == "__main__":
    unittest.main()
