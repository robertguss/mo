#!/usr/bin/env python3
"""Reports the duplicated code and long functions a change adds to src/ and lib/, using qgate.

Run from the repository root:
  python3 ci/qgate_report.py --qgate PATH BASE    compare HEAD with its merge base with BASE
  python3 ci/qgate_report.py --findings FILE      report from a saved `qgate run --format json`

This is report-only: it always exits 0, so it can never fail CI, and the hosted workflow runs it
as its own job outside the required `check` status. Robert is watching its reports for two weeks
(MO-100) before deciding whether it should block.

How "new" is decided: qgate records every violation in the base commit as a baseline, then checks
the change against that baseline, so a finding is something the change added or made worse. Of
qgate's findings this keeps only two kinds:
  function-length  a function in src/ or lib/ that is newly over 50 lines, or grew while over
  duplication      a copied block of 50+ tokens with at least one copy in src/ or lib/
and drops the rest: file length (most of the long files are long on purpose), parse warnings
(qgate's C grammar trips on Mo's macros), the baseline approvals qgate asks of a developer, and
notes about violations that shrank. Settings are in ci/qgate.toml.

qgate 0.1.0 has no grammar for Mo, so in lib/ it finds neither long functions nor duplicates; lib/
is watched so that a later qgate that reads Mo reports there without a change here.

When GITHUB_STEP_SUMMARY is set, the report is also written to the job summary, and each finding
becomes a notice annotation on the pull request's diff.
"""

import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time

root = pathlib.Path(__file__).resolve().parent.parent
CONFIG = root / "ci/qgate.toml"
WATCHED = ("src/", "lib/")
PARTNER = re.compile(r"also at ([^\s:]+):\d+")


def watched(path):
    return path is not None and path.startswith(WATCHED)


def keep(finding):
    """True for a finding this report shows: see the module docstring."""
    if finding.get("severity") != "error":
        return False
    rule = finding.get("rule")
    if rule == "function-length":
        return watched(finding.get("path"))
    if rule == "duplication":
        partners = PARTNER.findall(finding.get("message", ""))
        return watched(finding.get("path")) or any(watched(p) for p in partners)
    return False


def select(findings):
    kept = [f for f in findings if keep(f)]
    return sorted(kept, key=lambda f: (f["rule"], f.get("path", ""), f.get("line") or 0))


def git(*args, cwd=root):
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True,
                          text=True).stdout.strip()


def run_qgate(qgate, base_ref):
    """Runs qgate on HEAD against a baseline of the merge base; returns its parsed JSON."""
    base = git("merge-base", base_ref, "HEAD")
    head = git("rev-parse", "HEAD")
    work = pathlib.Path(tempfile.mkdtemp(prefix="qgate-"))
    trees = []
    try:
        for name, commit in (("base", base), ("head", head)):
            git("worktree", "add", "--detach", str(work / name), commit)
            trees.append(work / name)
        shutil.copyfile(CONFIG, work / "base/qgate.toml")
        subprocess.run([qgate, "baseline", "create"], cwd=work / "base", check=True,
                       capture_output=True, timeout=120)
        for name in ("qgate.toml", "qgate.baseline"):
            shutil.copyfile(work / "base" / name, work / "head" / name)
        # The head tree staged on top of the base commit: qgate's commit stage then checks exactly
        # the change, against the base's baseline. Nothing is committed.
        git("reset", "--soft", base, cwd=work / "head")
        git("add", "qgate.toml", "qgate.baseline", cwd=work / "head")
        result = subprocess.run([qgate, "run", "--stage", "commit", "--format", "json"],
                                cwd=work / "head", capture_output=True, text=True, timeout=120)
        return json.loads(result.stdout)
    finally:
        for tree in trees:
            subprocess.run(["git", "worktree", "remove", "--force", str(tree)], cwd=root,
                           capture_output=True, check=False)
        shutil.rmtree(work, ignore_errors=True)


def location(finding):
    line = finding.get("line")
    return f"{finding['path']}:{line}" if line else finding["path"]


def text_report(kept, seconds):
    lines = [f"qgate (report only, never fails CI): {len(kept)} new finding(s) in src/ and lib/"
             f" in {seconds:.1f} s"]
    lines += [f"  {f['rule']}: {location(f)}: {f['message']}" for f in kept]
    return "\n".join(lines) + "\n"


def markdown_report(kept, seconds):
    out = ["## qgate: new duplication and long functions (report only)", ""]
    if not kept:
        out.append(f"This change adds no duplicated code or long functions in src/ or lib/"
                   f" ({seconds:.1f} s).")
        return "\n".join(out) + "\n"
    out.append(f"This change adds {len(kept)} finding(s) in src/ or lib/ ({seconds:.1f} s)."
               " They never fail CI; see MO-100.")
    out += ["", "| Rule | Where | What |", "| --- | --- | --- |"]
    for f in kept:
        message = f["message"].replace("|", "\\|")
        out.append(f"| {f['rule']} | `{location(f)}` | {message} |")
    return "\n".join(out) + "\n"


def annotation(finding):
    message = finding["message"].replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
    where = f"file={finding['path']}"
    if finding.get("line"):
        where += f",line={finding['line']}"
    return f"::notice {where},title=qgate {finding['rule']}::{message}"


def report(findings, seconds):
    kept = select(findings)
    sys.stdout.write(text_report(kept, seconds))
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        for finding in kept:
            print(annotation(finding))
        with open(summary, "a", encoding="utf-8") as out:
            out.write(markdown_report(kept, seconds))


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--findings", help="a saved `qgate run --format json` output")
    source.add_argument("--qgate", help="the qgate executable; needs BASE")
    parser.add_argument("base", nargs="?", help="the branch or commit the change is based on")
    args = parser.parse_args(argv)
    if args.qgate and not args.base:
        parser.error("--qgate needs BASE")
    start = time.monotonic()
    try:
        if args.findings:
            data = json.loads(pathlib.Path(args.findings).read_text(encoding="utf-8"))
        else:
            data = run_qgate(args.qgate, args.base)
        report(data["findings"], time.monotonic() - start)
    except Exception as error:  # noqa: BLE001 -- report-only: no failure may fail CI
        print(f"qgate report skipped: {type(error).__name__}: {error}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
