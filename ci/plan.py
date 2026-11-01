#!/usr/bin/env python3

import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile


RULES = (
    (r"^(src|tests|fuzz)/.*\.[ch]\Z", ("clang", "sanitize", "static"),
     "AGENTS.md: Checks and merging"),
    (r"^src/base/(prims|hostops)\.def\Z", ("static",), "AGENTS.md: Checks and merging"),
    (r"^docs/(agent-guide\.md|reference/README\.md|design/library\.md)\Z",
     ("drivers", "static"), "AGENTS.md: Checks and merging"),
    (r"^(?!.*\.md\Z)(tests/(cli|golden|errors)/|src/tools/|ci/check_[^/]*\.py\Z)",
     ("drivers",), "AGENTS.md: Tests"),
    (r"^(?!.*\.md\Z)(formal/|tests/core/|src/(core|lower)/)", ("lean",),
     "AGENTS.md: Checks and merging; ci/check.sh: lean"),
    (r"^src/(bytecode/(read|verify|verify-instruction)|base/(arena|decimal|utf8)|"
     r"prim/(number|prim|text|unicode)|host/(files|path)|heap/(heap|cell|types|table))\.c\Z",
     ("cbmc",), "cbmc/README.md; cbmc/*.c: real source includes"),
    (r"^cbmc/(?!.*\.md\Z)", ("cbmc",), "cbmc/README.md"),
    (r"^fuzz/(?!.*\.md\Z)", ("fuzz",), "ci/check.sh: fuzz"),
    (r"^ci/[^/]*\.(py|sh)\Z", ("drivers", "static"), "ci/check.sh: drivers and static"),
    (r"\.md\Z", (), "AGENTS.md: Checks and merging, docs-only review exemption"),
)
GROUPS = ("gcc", "clang", "sanitize", "fuzz", "drivers", "lean", "static", "cbmc")
TOOL_CALLS = (
    ("gcc", ("gcc",), ("gcc",)),
    ("clang", ("clang",), ("clang",)),
    ("sanitize", ("sanitize",), ("clang", "gcc")),
    ("build", ("drivers", "lean"), ("gcc", "clang")),
    ("build:lean", ("lean",), ("clang", "gcc")),
    ("drivers", ("drivers",), ("python3", "curl", "python3")),
    ("fuzz", ("fuzz",), ("clang",)),
    ("static:grammar", ("static",), ("bison", "clang", "gcc", "clang")),
    ("lean", ("lean",), ("lake", "python3")),
    ("static:tail", ("static",),
     ("python3", "python3", "python3", "python3", "clang-format", "clang-tidy")),
    ("cbmc", ("cbmc",), ("cbmc", "goto-cc", "goto-instrument")),
)
FUZZER_SOURCE = (
    "int LLVMFuzzerTestOneInput(const unsigned char *d, unsigned long n);\n"
    "int LLVMFuzzerTestOneInput(const unsigned char *d, unsigned long n) "
    "{ (void)d; (void)n; return 0; }\n"
)
FUZZER_FLAGS = ("-fsanitize=fuzzer",)
LAKE_HOMES = ("ELAN_HOME", "/opt/elan", "HOME/.elan")


class Parser(argparse.ArgumentParser):
    def error(self, message):
        self.print_usage(sys.stderr)
        self.exit(64, f"{self.prog}: error: {message}\n")


def git(*args, cwd=None):
    result = subprocess.run(["git", *args], cwd=cwd, capture_output=True, timeout=60)
    if result.returncode:
        detail = os.fsdecode(result.stderr).strip() or f"exit {result.returncode}"
        raise RuntimeError(f"git {' '.join(args)}: {detail}")
    return result.stdout


def probe(command, environment):
    try:
        return subprocess.run(command, env=environment, capture_output=True, text=True, timeout=15)
    except (OSError, subprocess.TimeoutExpired):
        return None


def missing_tools(groups):
    environment = dict(os.environ)
    missing = {}

    def require(label, satisfied, affected):
        if not satisfied:
            missing.setdefault(label, set()).update(affected)

    for context, affected, calls in TOOL_CALLS:
        affected = set(affected).intersection(groups)
        if not affected or context in ("build", "build:lean"):
            continue
        if context == "lean":
            homes = {"ELAN_HOME": environment.get("ELAN_HOME", ""),
                     "HOME/.elan": os.path.join(environment.get("HOME", ""), ".elan")}
            for candidate in LAKE_HOMES:
                home = homes.get(candidate, candidate)
                if home and os.access(os.path.join(home, "bin/lake"), os.X_OK):
                    environment["ELAN_HOME"] = home
                    environment["PATH"] = os.path.join(home, "bin") + os.pathsep + environment.get("PATH", "")
                    break
        tools = list(dict.fromkeys(calls))
        available = {tool: shutil.which(tool, path=environment.get("PATH", "")) for tool in tools}
        if context == "sanitize":
            require("clang or gcc", any(available.values()), affected)
        elif context == "static:grammar":
            require(tools[0], bool(available[tools[0]]), affected)
            require("clang or gcc", any(available[tool] for tool in tools[1:]), affected)
        else:
            for tool in tools:
                label = "clang-format 14.0.6" if tool == "clang-format" else tool
                present = bool(available[tool])
                if tool == "clang-format" and present:
                    result = probe([available[tool], "--version"], environment)
                    version = re.search(r"\bversion\s+(\S+)", result.stdout) if result else None
                    present = bool(result and result.returncode == 0 and version and version[1] == "14.0.6")
                require(label, present, affected)
        if context == "fuzz" and available["clang"]:
            with tempfile.TemporaryDirectory(prefix="mo-plan-fuzzer-") as temporary:
                source = pathlib.Path(temporary) / "probe.c"
                source.write_text(FUZZER_SOURCE)
                result = probe([available["clang"], *FUZZER_FLAGS, str(source), "-o",
                                str(pathlib.Path(temporary) / "probe")], environment)
            require("libFuzzer (clang -fsanitize=fuzzer)", bool(result and result.returncode == 0), affected)
    return {label: [group for group in GROUPS if group in affected]
            for label, affected in sorted(missing.items())}


def plan(args):
    root = os.fsdecode(git("rev-parse", "--show-toplevel")).removesuffix("\n")
    if not args.no_fetch:
        git("fetch", "-q", "origin", "main", cwd=root)
    base = os.fsdecode(git("rev-parse", "--verify", "--end-of-options", args.base + "^{commit}", cwd=root)).strip()
    head = os.fsdecode(git("rev-parse", "--verify", "HEAD^{commit}", cwd=root)).strip()
    merge_base = os.fsdecode(git("merge-base", base, head, cwd=root)).strip()
    behind = int(git("rev-list", "--count", f"{merge_base}..{base}", cwd=root))
    paths = set()
    for command in (("diff", "--name-only", "-z", "--no-renames", merge_base, head, "--"),
                    ("diff", "--cached", "--name-only", "-z", "--no-renames", head, "--"),
                    ("diff", "--name-only", "-z", "--no-renames", "--"),
                    ("ls-files", "--others", "--exclude-standard", "-z")):
        paths.update(os.fsdecode(path) for path in git(*command, cwd=root).split(b"\0") if path)
    groups = {}
    unmapped = []
    for path in sorted(paths):
        matches = [pulled for pattern, pulled, source in RULES if re.search(pattern, path, re.DOTALL)]
        if not matches:
            unmapped.append(path)
        for pulled in matches:
            for group in pulled:
                groups.setdefault(group, set()).add(path)
    docs_only = bool(paths) and all(path.endswith(".md") for path in paths) and not groups
    if paths and not docs_only:
        groups["gcc"] = paths
    groups = {group: sorted(groups[group]) for group in GROUPS if group in groups}
    missing = missing_tools(groups)
    skipped = [group for group in GROUPS if any(group in affected for affected in missing.values())]
    return {"base": base, "merge_base": merge_base, "behind": behind, "groups": groups,
            "unmapped": unmapped, "missing_tools": missing, "skipped_groups": skipped,
            "docs_only": docs_only, "command": "bash ci/check.sh " + " ".join(groups) if groups else ""}


def print_text(report):
    print(f"base: {report['base']}")
    print(f"merge-base: {report['merge_base']}")
    print(f"base ahead since merge-base: {report['behind']}")
    print(f"rebase needed: {'yes' if report['behind'] else 'no'}")
    for group, paths in report["groups"].items():
        print(f"{group}: {', '.join(json.dumps(path) for path in paths)}")
    for path in report["unmapped"]:
        print(f"unmapped: {json.dumps(path)}")
    for tool, groups in report["missing_tools"].items():
        print(f"missing: {tool} ({', '.join(groups)})")
    if report["docs_only"]:
        print("PR may merge without review (docs-only).")
        checks = "review-free eligibility: docs-only; no checks recommended"
    elif not report["groups"]:
        checks = "empty diff; no checks recommended"
    else:
        checks = f"recommended `{report['command']}`; checks not run"
    print(f"PR summary: {checks}; branch behind base: {report['behind']}; "
          f"tool-gated skips: {', '.join(report['skipped_groups']) or 'none'}.")


def main():
    parser = Parser(description="Recommend local checks from all branch and working-tree changes. "
                    "Fetch origin main first unless --no-fetch. No checks are run.",
                    epilog="Exit 0 means no rebase or tool-gated skips are needed, not that checks passed. "
                    "Exit 1 means rebase needed, tool-gated skip, or Git/fetch error. Usage errors exit 64.",
                    allow_abbrev=False)
    parser.add_argument("--base", default="origin/main", help="base ref (default: origin/main; no fallback)")
    parser.add_argument("--json", action="store_true", help="print the report as JSON")
    parser.add_argument("--no-fetch", action="store_true", help="do not run git fetch -q origin main")
    args = parser.parse_args()
    try:
        report = plan(args)
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"plan: {error}", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps(report))
    else:
        print_text(report)
    return int(report["behind"] != 0 or bool(report["skipped_groups"]))


if __name__ == "__main__":
    sys.exit(main())
