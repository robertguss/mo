#!/usr/bin/env python3

import json
import os
import pathlib
import re
import runpy
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parent.parent
PLAN = ROOT / "ci/plan.py"
GIT = shutil.which("git")
BASH = shutil.which("bash")
ORDER = ["gcc", "clang", "sanitize", "fuzz", "drivers", "lean", "static", "cbmc"]
KEYS = {"base", "merge_base", "behind", "groups", "unmapped", "missing_tools",
        "skipped_groups", "docs_only", "command"}
TOOLS = ("gcc", "clang", "bison", "python3", "curl", "clang-format", "clang-tidy",
         "lake", "cbmc", "goto-cc", "goto-instrument")
STUB = r'''
import json
import os
import pathlib
import sys

name = pathlib.Path(sys.argv[0]).name
if os.environ.get("TOOL_LOG"):
    with open(os.environ["TOOL_LOG"], "a") as output:
        output.write(json.dumps([name, sys.argv[1:]]) + "\n")
if name == "clang-format":
    print("clang-format version " + os.environ.get("FORMAT_VERSION", "14.0.6"))
if name in os.environ.get("FAIL_TOOLS", "").split():
    print("deliberate tool failure", file=sys.stderr)
    sys.exit(1)
if name == "make" and any(arg in ("test", "sanitize", "test-executables", "sanitize-executables")
                          for arg in sys.argv[1:]):
    compiler = next(arg[3:] for arg in sys.argv[1:] if arg.startswith("CC="))
    mode = "sanitize" if any(arg.startswith("sanitize") for arg in sys.argv[1:]) else "debug"
    for binary in ("mo", "tests/test_format.test", "tests/test_bytecode_text.test",
                   "tests/test_codegen.test", "tests/test_unicode16.test", "tests/test_prim.test",
                   "tests/test_vm.test", "tests/test_lower.test", "tests/test_heap.test",
                   "tests/test_core_check.test", "tests/test_host.test"):
        target = pathlib.Path("build") / compiler / mode / binary
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(f"#!{sys.executable}\n")
        target.chmod(0o755)
'''


class Repository(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="mo-plan-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = pathlib.Path(self.temporary.name)
        self.root = self.directory / "work"
        self.root.mkdir()
        self.bin = self.directory / "bin"
        self.bin.mkdir()
        self.home = self.directory / "home"
        self.home.mkdir()
        self.environment = dict(os.environ, PATH=str(self.bin), HOME=str(self.home),
                                ELAN_HOME=str(self.bin.parent / "elan"),
                                GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull,
                                GIT_AUTHOR_NAME="Planner test", GIT_AUTHOR_EMAIL="plan@example.test",
                                GIT_COMMITTER_NAME="Planner test",
                                GIT_COMMITTER_EMAIL="plan@example.test",
                                PYTHONDONTWRITEBYTECODE="1", CI="", LC_ALL="C")
        for key in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_CONFIG_COUNT",
                    "FAIL_TOOLS", "FORMAT_VERSION", "TOOL_LOG"):
            self.environment.pop(key, None)
        (self.bin / "git").symlink_to(GIT)
        for tool in TOOLS:
            self.tool(self.bin / tool)
        self.git("init", "-q", "-b", "main")
        self.write("README.md", "base\n")
        self.git("add", ".")
        self.git("commit", "-qm", "base")
        self.base = self.git("rev-parse", "HEAD").strip()
        self.git("update-ref", "refs/remotes/origin/main", self.base)
        self.git("switch", "-qc", "feature")

    def tool(self, path):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f"#!{sys.executable}\n" + STUB)
        path.chmod(0o755)

    def write(self, path, contents="changed\n"):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(contents)

    def git(self, *args, cwd=None):
        result = subprocess.run([GIT, *args], cwd=cwd or self.root, env=self.environment,
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def cli(self, *args, cwd=None, fetch=False, script=PLAN):
        command = [sys.executable, str(script)]
        if not fetch:
            command.append("--no-fetch")
        result = subprocess.run([*command, *args], cwd=cwd or self.root,
                                env=self.environment, capture_output=True, text=True, timeout=30)
        return result

    def report(self, *args, status=0, **kwargs):
        result = self.cli("--json", *args, **kwargs)
        self.assertEqual(result.returncode, status, result.stdout + result.stderr)
        self.assertEqual(result.stderr, "")
        report = json.loads(result.stdout)
        self.assertEqual(set(report), KEYS)
        self.assertEqual(list(report["groups"]), [group for group in ORDER if group in report["groups"]])
        return report

    def assert_paths(self, report, paths, groups, unmapped=()):
        expected = {group: sorted(paths) for group in ORDER if group in groups}
        self.assertEqual(report["groups"], expected)
        self.assertEqual(report["unmapped"], sorted(unmapped))
        self.assertEqual(report["missing_tools"], {})
        self.assertEqual(report["skipped_groups"], [])
        self.assertEqual(report["command"], "bash ci/check.sh " + " ".join(expected) if expected else "")


class Paths(Repository):
    def test_mapping(self):
        cases = [
            ("src/check/signatures.c", "gcc clang sanitize static"),
            ("tests/header.h", "gcc clang sanitize static"),
            ("fuzz/fuzz_reader.c", "gcc clang sanitize fuzz static"),
            ("fuzz/corpus/reader/seed", "gcc fuzz"),
            ("src/base/prims.def", "gcc static"),
            ("src/base/hostops.def", "gcc static"),
            ("docs/agent-guide.md", "gcc drivers static"),
            ("docs/reference/README.md", "gcc drivers static"),
            ("docs/design/library.md", "gcc drivers static"),
            ("tests/cli/check/input.mo", "gcc drivers"),
            ("tests/golden/hello.mo", "gcc drivers"),
            ("tests/errors/e0001/main.mo", "gcc drivers"),
            ("src/tools/main.c", "gcc clang sanitize drivers static"),
            ("ci/check_new.py", "gcc drivers static"),
            ("ci/new.py", "gcc drivers static"),
            ("ci/new.sh", "gcc drivers static"),
            ("formal/Main.lean", "gcc lean"),
            ("tests/core/sample.core", "gcc lean"),
            ("src/core/core.h", "gcc clang sanitize lean static"),
            ("src/lower/expressions.c", "gcc clang sanitize lean static"),
            ("cbmc/prove.sh", "gcc cbmc"),
        ]
        cbmc = ["bytecode/read.c", "base/arena.c", "bytecode/verify.c",
                "bytecode/verify-instruction.c", "base/decimal.c", "base/utf8.c",
                "prim/number.c", "prim/prim.c", "prim/text.c", "prim/unicode.c",
                "host/files.c", "host/path.c", "heap/heap.c", "heap/cell.c",
                "heap/types.c", "heap/table.c"]
        cases += [("src/" + path, "gcc clang sanitize static cbmc") for path in cbmc]
        for path, groups in cases:
            with self.subTest(path=path):
                self.write(path)
                try:
                    self.assert_paths(self.report(), [path], groups.split())
                finally:
                    (self.root / path).unlink()

    def test_empty_and_docs_only(self):
        report = self.report()
        self.assert_paths(report, [], [])
        self.assertFalse(report["docs_only"])
        for path in ("README.md", "docs/design/notes.md", "cbmc/README.md", "formal/notes.md",
                     "fuzz/README.md", "tests/cli/README.md"):
            self.write(path)
        report = self.report()
        self.assert_paths(report, [], [])
        self.assertTrue(report["docs_only"])
        result = self.cli()
        self.assertIn("PR may merge without review", result.stdout)
        self.assertIn("review-free", result.stdout.splitlines()[-1])
        self.assertNotIn("passed", result.stdout.lower())

    def test_mixed_docs_and_unmapped(self):
        paths = ["docs/notes.md", "unknown/new.file"]
        for path in paths:
            self.write(path)
        report = self.report()
        self.assert_paths(report, paths, ["gcc"], [paths[1]])
        self.assertFalse(report["docs_only"])
        result = self.cli()
        self.assertIn("unmapped", result.stdout.lower())
        self.assertIn(paths[1], result.stdout)

    def test_union_and_odd_names(self):
        committed = "tests/cli/committed.mo"
        deleted = "tests/golden/deleted.mo"
        renamed = "tests/errors/old name.mo"
        for path in (deleted, renamed):
            self.write(path, "original\n")
        self.git("add", ".")
        self.git("commit", "-qm", "tracked fixtures")
        self.base = self.git("rev-parse", "HEAD").strip()
        self.git("update-ref", "refs/remotes/origin/main", self.base)
        self.write(committed)
        self.git("add", ".")
        self.git("commit", "-qm", "branch fixture")
        (self.root / deleted).unlink()
        moved = "tests/cli/new\tname\n\"\\.mo"
        self.git("mv", renamed, moved)
        staged = "tests/cli/staged.mo"
        self.write(staged)
        self.git("add", staged)
        (self.root / staged).unlink()
        untracked = "tests/cli/-untracked.mo"
        self.write(untracked)
        self.write(".gitignore", "ignored/\n")
        self.write("ignored/hidden.c")
        paths = [committed, deleted, renamed, moved, staged, untracked]
        report = self.report()
        self.assertEqual(report["groups"]["drivers"], sorted(paths))
        self.assertEqual(report["unmapped"], [".gitignore"])
        self.assertNotIn("ignored/hidden.c", report["groups"]["gcc"])
        self.assertEqual(len(report["groups"]["gcc"]), len(paths) + 1)

    def test_staged_edit_undone_in_worktree(self):
        self.write("tests/cli/staged.mo", "original\n")
        self.git("add", ".")
        self.git("commit", "-qm", "fixture")
        self.git("update-ref", "refs/remotes/origin/main", self.git("rev-parse", "HEAD").strip())
        self.write("tests/cli/staged.mo", "staged\n")
        self.git("add", ".")
        self.write("tests/cli/staged.mo", "original\n")
        self.assert_paths(self.report(), ["tests/cli/staged.mo"], ["gcc", "drivers"])

    def test_committed_rename_and_unstaged_edit(self):
        old = "tests/cli/old.mo"
        new = "tests/golden/new.mo"
        self.write(old, "original\n")
        self.write("tests/errors/edited.mo", "original\n")
        self.git("add", ".")
        self.git("commit", "-qm", "fixtures")
        self.git("update-ref", "refs/remotes/origin/main", self.git("rev-parse", "HEAD").strip())
        (self.root / "tests/golden").mkdir()
        self.git("mv", old, new)
        self.git("commit", "-qm", "rename")
        self.write("tests/errors/edited.mo", "unstaged\n")
        self.assert_paths(self.report(), [old, new, "tests/errors/edited.mo"], ["gcc", "drivers"])

    def test_nested_cwd_and_copied_script(self):
        copied = self.directory / "plan-copy.py"
        shutil.copyfile(PLAN, copied)
        self.write("tests/cli/new.mo")
        report = self.report(cwd=self.root / "tests/cli", script=copied)
        self.assert_paths(report, ["tests/cli/new.mo"], ["gcc", "drivers"])


class GitState(Repository):
    def test_asymmetric_divergence_and_base_override(self):
        self.git("switch", "-q", "main")
        for index in range(2):
            self.write(f"base-only-{index}.c")
            self.git("add", ".")
            self.git("commit", "-qm", "base moved")
        base = self.git("rev-parse", "HEAD").strip()
        self.git("update-ref", "refs/remotes/origin/main", base)
        self.git("switch", "-q", "feature")
        self.write("tests/cli/new.mo")
        self.git("add", ".")
        self.git("commit", "-qm", "feature")
        report = self.report(status=1)
        self.assertEqual((report["base"], report["merge_base"], report["behind"]), (base, self.base, 2))
        self.assert_paths(report, ["tests/cli/new.mo"], ["gcc", "drivers"])
        text = self.cli()
        self.assertEqual(text.returncode, 1)
        self.assertIn(self.base, text.stdout)
        self.assertIn(base, text.stdout)
        self.assertIn("rebase needed: yes", text.stdout)
        alternate = self.report("--base", self.base)
        self.assertEqual(alternate["behind"], 0)
        self.assertEqual(alternate["base"], self.base)

    def test_fetch_default_and_no_fetch(self):
        remote = self.directory / "remote.git"
        self.git("init", "-q", "--bare", str(remote))
        self.git("remote", "add", "origin", str(remote))
        self.git("push", "-q", "origin", "main")
        self.git("switch", "-q", "main")
        self.write("remote.md")
        self.git("add", ".")
        self.git("commit", "-qm", "remote advanced")
        remote_sha = self.git("rev-parse", "HEAD").strip()
        self.git("push", "-q", "origin", "main")
        self.git("update-ref", "refs/remotes/origin/main", self.base)
        self.git("switch", "-q", "feature")
        self.assertEqual(self.report()["base"], self.base)
        report = self.report(fetch=True, status=1)
        self.assertEqual((report["base"], report["behind"]), (remote_sha, 1))
        self.git("update-ref", "refs/remotes/origin/main", self.base)
        report = self.report("--base", self.base, fetch=True)
        self.assertEqual(report["base"], self.base)
        self.assertEqual(self.git("rev-parse", "origin/main").strip(), remote_sha)

    def test_operational_and_usage_errors(self):
        for args, fetch, cwd in [([], True, self.root), (["--base", "absent"], False, self.root),
                                 (["--base=--help"], False, self.root),
                                 (["--base", "--all"], False, self.root),
                                 ([], False, self.directory)]:
            with self.subTest(args=args, fetch=fetch, cwd=cwd):
                result = self.cli("--json", *args, fetch=fetch, cwd=cwd)
                self.assertEqual(result.returncode, 64 if args == ["--base", "--all"] else 1)
                self.assertEqual(result.stdout, "")
                self.assertTrue(result.stderr)
        self.git("update-ref", "-d", "refs/remotes/origin/main")
        result = self.cli("--json")
        self.assertEqual(result.returncode, 1)
        self.assertTrue(result.stderr)
        for args in (["--wat"], ["extra"], ["--base"]):
            result = self.cli(*args)
            self.assertEqual(result.returncode, 64)
            self.assertEqual(result.stdout, "")
        help_result = self.cli("--help")
        self.assertEqual(help_result.returncode, 0)
        for word in ("fetch", "--base", "--json", "--no-fetch", "64", "skip", "rebase"):
            self.assertIn(word, help_result.stdout)

    def test_diff_operational_error_is_not_an_empty_plan(self):
        (self.bin / "git").unlink()
        (self.bin / "git").write_text(f'''#!{sys.executable}
import os
import sys
if sys.argv[1] == "diff":
    print("deliberate diff failure", file=sys.stderr)
    sys.exit(23)
os.execv({GIT!r}, [{GIT!r}, *sys.argv[1:]])
''')
        (self.bin / "git").chmod(0o755)
        result = self.cli("--json")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")
        self.assertIn("deliberate diff failure", result.stderr)

    def test_base_is_pinned_before_ref_moves(self):
        self.write("tests/cli/branch.mo")
        self.git("add", ".")
        self.git("commit", "-qm", "feature")
        head = self.git("rev-parse", "HEAD").strip()
        (self.bin / "git").unlink()
        (self.bin / "git").write_text(f'''#!{sys.executable}
import subprocess
import sys
result = subprocess.run([{GIT!r}, *sys.argv[1:]], capture_output=True)
if sys.argv[-1] == "origin/main^{{commit}}":
    subprocess.run([{GIT!r}, "update-ref", "refs/remotes/origin/main", {head!r}], check=True)
sys.stdout.buffer.write(result.stdout)
sys.stderr.buffer.write(result.stderr)
sys.exit(result.returncode)
''')
        (self.bin / "git").chmod(0o755)
        report = self.report()
        self.assertEqual((report["base"], report["merge_base"], report["behind"]), (self.base, self.base, 0))
        self.assert_paths(report, ["tests/cli/branch.mo"], ["gcc", "drivers"])

    def test_text_summary(self):
        self.write("ci/new.py")
        result = self.cli()
        self.assertEqual(result.returncode, 0)
        for group in ("gcc", "drivers", "static"):
            self.assertRegex(result.stdout, rf"(?m)^{group}: .*ci/new.py")
        self.assertIn("rebase needed: no", result.stdout)
        lines = result.stdout.splitlines()
        self.assertTrue(lines[-1].startswith("PR summary:"))
        self.assertEqual(sum(line.startswith("PR summary:") for line in lines), 1)
        self.assertIn("bash ci/check.sh gcc drivers static", lines[-1])
        self.assertIn("not run", lines[-1])
        self.assertNotIn("passed", result.stdout.lower())


class Tools(Repository):
    def run_shell(self, *groups):
        runner = self.directory / "runner"
        if runner.exists():
            shutil.rmtree(runner)
        (runner / "ci").mkdir(parents=True)
        shutil.copyfile(ROOT / "ci/check.sh", runner / "ci/check.sh")
        (runner / "ci/check_core_fixtures.sh").write_text("exit 0\n")
        (runner / "cbmc").mkdir()
        (runner / "cbmc/prove.sh").write_text("exit 0\n")
        for tool in ("dirname", "getconf", "mktemp", "rm", "wc", "sleep", "cat", "pkill", "mkdir", "env", "sh", "bash"):
            target = self.bin / tool
            if not target.exists():
                target.symlink_to(shutil.which(tool))
        self.tool(self.bin / "make")
        result = subprocess.run([BASH, "ci/check.sh", *groups], cwd=runner,
                                env=self.environment, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def all_groups(self):
        for path in ("src/check/new.c", "ci/new.py", "fuzz/corpus/seed", "formal/New.lean", "cbmc/new.c"):
            self.write(path)

    def test_each_tool_association_and_shell_gates(self):
        self.all_groups()
        cases = [
            ("gcc", {"gcc": ["gcc"]}),
            ("clang", {"clang": ["clang", "fuzz"]}),
            ("bison", {"bison": ["static"]}),
            ("python3", {"python3": ["drivers", "lean", "static"]}),
            ("curl", {"curl": ["drivers"]}),
            ("clang-format", {"clang-format 14.0.6": ["static"]}),
            ("clang-tidy", {"clang-tidy": ["static"]}),
            ("cbmc", {"cbmc": ["cbmc"]}),
            ("goto-cc", {"goto-cc": ["cbmc"]}),
            ("goto-instrument", {"goto-instrument": ["cbmc"]}),
        ]
        if not os.access("/opt/elan/bin/lake", os.X_OK):
            cases.append(("lake", {"lake": ["lean"]}))
        for tool, missing in cases:
            with self.subTest(tool=tool):
                (self.bin / tool).unlink()
                try:
                    expected = [group for group in ORDER if any(group in affected for affected in missing.values())]
                    report = self.report(status=1)
                    self.assertEqual(report["missing_tools"], missing)
                    self.assertEqual(report["skipped_groups"], expected)
                    for group in expected:
                        self.assertIn("skipped:", self.run_shell(group))
                    if tool in ("gcc", "clang"):
                        for group in ("sanitize", "static"):
                            self.assertNotIn("skipped:", self.run_shell(group))
                    text = self.cli()
                    self.assertEqual(text.returncode, 1)
                    self.assertIn("missing:", text.stdout)
                    self.assertIn(report["command"], text.stdout.splitlines()[-1])
                finally:
                    self.tool(self.bin / tool)

    def test_no_compiler_and_no_built_outputs(self):
        self.all_groups()
        (self.bin / "gcc").unlink()
        (self.bin / "clang").unlink()
        report = self.report(status=1)
        self.assertEqual(report["missing_tools"], {
            "clang": ["clang", "fuzz"],
            "clang or gcc": ["sanitize", "drivers", "lean", "static"],
            "gcc": ["gcc"],
        })
        for group in ("sanitize", "drivers", "lean", "static"):
            with self.subTest(group=group):
                self.assertIn("skipped:", self.run_shell(group))

    def test_format_version_exact_and_fuzzer_compile_only(self):
        self.write("fuzz/new.c")
        for version in ("14.0.60", "14.0.6.1", "15.0.0", "14.0.6git"):
            with self.subTest(version=version):
                self.environment["FORMAT_VERSION"] = version
                report = self.report(status=1)
                self.assertEqual(report["missing_tools"], {"clang-format 14.0.6": ["static"]})
        self.environment["FORMAT_VERSION"] = "14.0.6"
        log = self.directory / "tool-log"
        self.environment["TOOL_LOG"] = str(log)
        self.assertEqual(self.report()["missing_tools"], {})
        events = [json.loads(line) for line in log.read_text().splitlines()]
        clang = [args for tool, args in events if tool == "clang"]
        self.assertEqual(len(clang), 1)
        self.assertEqual(clang[0][0], "-fsanitize=fuzzer")
        self.assertEqual(clang[0][2], "-o")
        self.assertTrue(clang[0][1].endswith("/probe.c"))
        self.assertTrue(clang[0][3].endswith("/probe"))
        self.environment["FAIL_TOOLS"] = "clang"
        report = self.report(status=1)
        self.assertEqual(report["missing_tools"], {"libFuzzer (clang -fsanitize=fuzzer)": ["fuzz"]})
        self.assertEqual(report["skipped_groups"], ["fuzz"])
        self.assertIn("skipped:", self.run_shell("fuzz"))

    def test_lake_search_precedence_and_later_path(self):
        self.write("src/lower/new.c")
        self.write("cbmc/new.c")
        configured = pathlib.Path(self.environment["ELAN_HOME"]) / "bin"
        moved = ("python3", "clang-format", "clang-tidy", "cbmc", "goto-cc", "goto-instrument")
        for tool in ("lake", *moved):
            self.tool(configured / tool)
        for tool in moved:
            (self.bin / tool).unlink()
        report = self.report()
        self.assertEqual(report["missing_tools"], {})
        self.assertNotIn("skipped:", self.run_shell(*report["groups"]))
        home_bin = self.home / ".elan/bin"
        for tool in ("lake", *moved):
            self.tool(home_bin / tool)
        (configured / "python3").unlink()
        report = self.report(status=1)
        self.assertEqual(report["missing_tools"], {"python3": ["lean", "static"]})
        self.assertIn("skipped:", self.run_shell("lean", "static"))
        if not os.access("/opt/elan/bin/lake", os.X_OK):
            (configured / "lake").unlink()
            self.assertEqual(self.report()["missing_tools"], {})
            self.assertNotIn("skipped:", self.run_shell("lean", "static", "cbmc"))


class RunnerContract(unittest.TestCase):
    def test_literal_have_inventory_and_contexts(self):
        planner = runpy.run_path(str(PLAN))
        shell = (ROOT / "ci/check.sh").read_text()
        order = re.search(r'^all_groups=" ([^"]*) "$', shell, re.MULTILINE)[1].split()
        self.assertEqual(tuple(order), planner["GROUPS"])
        inventory = []
        context = None
        static_count = 0
        for line in shell.splitlines():
            if line.lstrip().startswith("#"):
                continue
            section = re.fullmatch(r"if want (\w+); then", line)
            if section:
                context = section[1]
                if context == "static":
                    static_count += 1
                    context += ":grammar" if static_count == 1 else ":tail"
            elif line == 'if [[ -n "$groups" ]] && { want drivers || want lean; }; then':
                context = "build"
            elif context == "build" and line == '\tif want lean; then':
                context = "build:lean"
            calls = re.findall(r"\bhave ([a-z][a-z0-9-]*)\b", line)
            self.assertEqual(len(calls), len(re.findall(r"\bhave\b(?!\()", line)), line)
            for tool in calls:
                self.assertIsNotNone(context, line)
                if not inventory or inventory[-1][0] != context:
                    affected = (("drivers", "lean") if context == "build" else
                                ("lean",) if context == "build:lean" else (context.split(":")[0],))
                    inventory.append((context, affected, []))
                inventory[-1][2].append(tool)
        self.assertEqual([(context, affected, tuple(calls)) for context, affected, calls in inventory],
                         list(planner["TOOL_CALLS"]))

    def test_fuzzer_probe_and_lake_discovery(self):
        planner = runpy.run_path(str(PLAN))
        shell = (ROOT / "ci/check.sh").read_text()
        source = re.search(r"printf '([^']*)' >\"\$dir/probe.c\"", shell)[1]
        self.assertEqual(source.replace("\\n", "\n"), planner["FUZZER_SOURCE"])
        flags = re.search(r'err=\$\(clang (.*?) "\$dir/probe.c" -o "\$dir/probe" 2>&1\)', shell)[1]
        self.assertEqual(tuple(shlex.split(flags)), planner["FUZZER_FLAGS"])
        homes = re.search(r"^for elan_home in (.*); do$", shell, re.MULTILINE)[1]
        self.assertEqual(tuple(shlex.split(homes)),
                         tuple({"ELAN_HOME": "${ELAN_HOME:-}", "HOME/.elan": "$HOME/.elan"}.get(home, home)
                               for home in planner["LAKE_HOMES"]))
        self.assertIn('if [[ -n "$elan_home" && -x "$elan_home/bin/lake" ]]; then', shell)
        self.assertIn('export ELAN_HOME="$elan_home" PATH="$elan_home/bin:$PATH"\n\t\tbreak', shell)


if __name__ == "__main__":
    unittest.main()
