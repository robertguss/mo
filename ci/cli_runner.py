import json
import os
import pathlib
import resource
import subprocess
import tempfile

from cli_common import records, root, run


def small_stack():
    _, hard = resource.getrlimit(resource.RLIMIT_STACK)
    resource.setrlimit(resource.RLIMIT_STACK, (1 << 20, hard))


def runner(binary):
    with tempfile.TemporaryDirectory(prefix="mo-runner-") as directory:
        path = pathlib.Path(directory) / "main.mo"
        path.write_text('module Main\nfn main(console: Console) : Int\n'
                        'console.print("hello 日本語")\n37\nend\n')
        checked = run(binary, ["check", str(path)], 0)
        assert not checked.stdout and not checked.stderr
        result = run(binary, ["run", str(path)], 37)
        assert result.stdout == "hello 日本語\n".encode() and not result.stderr
        # Emission writes indexed text, not an opaque binary or an execution
        # result. Both forms preserve the original filename and entry metadata.
        core = run(binary, ["check", "--emit=core", str(path)], 0)
        bytecode = run(binary, ["check", "--emit=bytecode", str(path)], 0)
        assert core.stdout.startswith(b"(core ") and not core.stderr
        assert b"Main.main.entry" in core.stdout and str(path).encode() in core.stdout
        assert b"Main.main.entry" in bytecode.stdout and not bytecode.stderr
        for name, result in (("core", core), ("bytecode", bytecode)):
            emitted = pathlib.Path(directory) / f"program.{name}"
            emitted.write_bytes(result.stdout)
            decoded = run(binary.parent / "tests/test_cli.test",
                          [f"--roundtrip-{name}", str(emitted)], 0)
            assert not decoded.stdout and not decoded.stderr

        # The shipped sample must work unchanged, including its fake-console
        # test calling the same main that receives real command-line handles.
        greeting = "examples/greet.mo"
        checked = run(binary, ["check", greeting], 0)
        assert not checked.stdout and not checked.stderr
        for values, expected in (([], "world"), (["Ada", "Grace"], "Ada"),
                                  (["日本語", "ignored"], "日本語"), (["--flag"], "--flag")):
            result = run(binary, ["run", greeting, *values], 0)
            assert result.stdout == f"Hello, {expected}!\n".encode() and not result.stderr
        greet_text = run(binary, ["test", greeting], 0)
        assert not greet_text.stderr and greet_text.stdout.endswith(b"3 passed, 0 failed, 0 stopped\n")
        items = records(run(binary, ["test", "--json", greeting], 0), "test")
        assert [(r["name"], r["outcome"]) for r in items[1:-1]] == [
            ("Greet.greeting.example.1", "passed"),
            ("Greet.greeting.example.2", "passed"), ("Greet.test.1", "passed")]
        assert items[-1] == {"record": "totals", "errors": 0, "dropped": 0,
                             "passed": 3, "failed": 0, "stopped": 0, "status": 0}

        finding = "examples/find.mo"
        checked = run(binary, ["check", finding], 0)
        assert not checked.stdout and not checked.stderr
        matched = run(binary, ["run", finding, "--count", "module Find", finding], 0)
        assert matched.stdout == b"examples/find.mo: 1\n" and not matched.stderr
        unmatched = run(binary, ["run", finding, "no-such-phrase-anywhere", finding], 1)
        assert not unmatched.stdout and not unmatched.stderr
        usage = run(binary, ["run", finding], 2)
        assert not usage.stdout and usage.stderr == (
            b"find: no phrase to find\n"
            b"usage: find [--ignore-case] [--count] PHRASE FILE...\n")
        items = records(run(binary, ["test", "--json", finding], 0), "test")
        assert [(r["name"], r["outcome"]) for r in items[1:-1]] == [
            ("Find.parse_args.example.1", "passed"),
            ("Find.parse_args.example.2", "passed"),
            ("Find.parse_args.example.3", "passed"),
            ("Find.parse_args.example.4", "passed"),
            ("Find.parse_args.example.5", "passed"),
            ("Find.matches.example.1", "passed"),
            ("Find.matches.example.2", "passed"),
            ("Find.test.1", "passed"),
            ("Find.test.2", "passed")]
        assert items[-1] == {"record": "totals", "errors": 0, "dropped": 0,
                             "passed": 9, "failed": 0, "stopped": 0, "status": 0}

        counting = "examples/wc.mo"
        checked = run(binary, ["check", counting], 0)
        assert not checked.stdout and not checked.stderr
        single = run(binary, ["run", counting, counting], 0)
        assert single.stdout == b"     73     276    2053 examples/wc.mo\n" and not single.stderr
        missing = run(binary, ["run", counting, counting, "no-such-file.mo"], 1)
        assert missing.stdout == (
            b"     73     276    2053 examples/wc.mo\n"
            b"     73     276    2053 total\n"
        ) and missing.stderr == b"wc: no-such-file.mo: no such file\n"
        items = records(run(binary, ["test", "--json", counting], 0), "test")
        assert [(r["name"], r["outcome"]) for r in items[1:-1]] == [
            ("Wc.count.example.1", "passed"),
            ("Wc.count.example.2", "passed"),
            ("Wc.count.example.3", "passed"),
            ("Wc.row.example.1", "passed"),
            ("Wc.test.1", "passed")]
        assert items[-1] == {"record": "totals", "errors": 0, "dropped": 0,
                             "passed": 5, "failed": 0, "stopped": 0, "status": 0}

        for parameters in ("_args: Args", "_clock: Clock, _console: Console"):
            path.write_text(f"module Main\nfn main({parameters})\nend\n")
            result = run(binary, ["run", str(path)], 0)
            assert not result.stdout and not result.stderr

        # Generic native aliases must use the instantiated element type.
        # Spec-only aliases stay lexical to specs, including anchor values.
        path.write_text('module Main\nfn first(values: List(T)) : Option(T)\nexamples\n'
                        'first(Args.of(["Ada", "Grace"]).list) == Some("Ada")\nend\n'
                        'pick = List.first\npick(values)\nend\n'
                        'fn keep(value: Int) : Int\npromise "keeps input"\n'
                        'result == value and Console.fake().printed == []\n'
                        'allows value: (Args.of(["abcd"]).list.first or "").byte_count, result: 4\n'
                        'forbids value: 4, result: 5\nend\nvalue\nend\n'
                        'fn main(console: Console, _random: Random, _files: Files, _env: Env, '
                        'args: Args, _read: ReadFiles, _clock: Clock) : Int\n'
                        'console.print(first(args.list) or "world")\n17\nend\n'
                        'test "fake aliases and snapshots"\n'
                        'make = Console.fake\narguments = Args.of\nread = Args.list\n'
                        'output = Console.printed\nconsole = make()\nalias = console\n'
                        'args = arguments(["λ\\u{0}owned", "ignored"])\n'
                        'console.print(first(read(args)) or "missing")\n'
                        'before = output(console)\nalias.print("after")\n'
                        'assert before == ["λ\\u{0}owned"]\n'
                        'assert output(console) == ["λ\\u{0}owned", "after"]\n'
                        'assert first([17, 29]) == Some(17)\n'
                        'assert first([true, false]) == Some(true)\nend\n')
        result = run(binary, ["run", str(path), "real", "ignored"], 17)
        assert result.stdout == b"real\n" and not result.stderr
        items = records(run(binary, ["test", "--json", str(path)], 0), "test")
        assert [(r["name"], r["outcome"]) for r in items[1:-1]] == [
            ("Main.first.example.1", "passed"), ("Main.keep.promise.1", "passed"),
            ("Main.test.1", "passed")]
        assert items[-1]["passed"] == 3 and items[-1]["errors"] == 0
        for name in ("core", "bytecode"):
            emitted = pathlib.Path(directory) / f"capabilities.{name}"
            emitted.write_bytes(run(binary, ["check", f"--emit={name}", str(path)], 0).stdout)
            decoded = run(binary.parent / "tests/test_cli.test",
                          [f"--roundtrip-{name}", str(emitted)], 0)
            assert not decoded.stdout and not decoded.stderr

        for body in ('factory = Console.fake\nfn main()\nend',
                     'factory = fn() Args.of([]) end\nfn main()\nend',
                     'fn helper() : Console\nConsole.fake()\nend\n'
                     'test "ordinary helper stays ordinary"\n_console = helper()\nend\n'
                     'fn main()\nend',
                     'fn main(console: Console)\n_printed = console.printed\nend'):
            path.write_text("module Main\n" + body + "\n")
            items = records(run(binary, ["check", "--json", str(path)], 1), "check")
            assert items[1]["code"] == "E0098" and items[-1]["errors"] == 1

        for body, status, reason in (
            ("fn main()\nend", 0, None),
            ("fn main() : Int\n0\nend", 0, None),
            ("fn main() : Int\n255\nend", 255, None),
            ("fn main() : Int\n-1\nend", 70, b"exit status outside 0 to 255"),
            ("fn main() : Int\n256\nend", 70, b"exit status outside 0 to 255"),
            ("fn main() : Int\n9223372036854775807 + 1\nend", 70, b"overflow: Int"),
            ('fn main()\nstop "chosen stop"\nend', 70, b"chosen stop"),
        ):
            path.write_text("module Main\n" + body + "\n")
            result = run(binary, ["run", str(path)], status)
            assert not result.stdout
            if reason is None:
                assert not result.stderr
            else:
                assert result.stderr.startswith(b"bug: ") and reason in result.stderr
                assert str(path).encode() in result.stderr and b"Main.main" in result.stderr

        for body, status, expected_stdout, expected_stderr in (
            ("fn main() : Result(Unit, FileError)\nOk(Unit)\nend", 0, b"", b""),
            ('fn main() : Result(Unit, FileError)\nError(NotFound(path: "notes.txt"))\nend',
             1, b"", b"notes.txt: no such file\n"),
            ('enum Problem\nBadInput(Text)\nend\n'
             'fn to_text(p: Problem) : Text\ncase p\nBadInput(msg): "bad: #{msg}"\nend\nend\n'
             'fn main(console: Console) : Result(Unit, Problem)\n'
             'console.print("before")\nError(BadInput("flag"))\nend',
             1, b"before\n", b"bad: flag\n"),
        ):
            path.write_text("module Main\n" + body + "\n")
            checked = run(binary, ["check", str(path)], 0)
            assert not checked.stdout and not checked.stderr
            result = run(binary, ["run", str(path)], status)
            assert result.stdout == expected_stdout and result.stderr == expected_stderr
        for body, code in (
            ("fn main() : Result(Unit, Bytes)\nOk(Unit)\nend", "E0080"),
            ("fn main() : Result(Int, FileError)\nOk(0)\nend", "E0079"),
        ):
            path.write_text("module Main\n" + body + "\n")
            items = records(run(binary, ["check", "--json", str(path)], 1), "check")
            assert items[1]["code"] == code and items[-1]["errors"] == 1

        # Tail calls replace a frame; a non-tail call must consume both limits.
        for tail, status in ((True, 0), (False, 70)):
            body = ("fn recurse(n: Int) : Int\nif n == 0: 0 else: "
                    + ("" if tail else "1 + ") + "recurse(n - 1)\nend\n"
                    "fn main() : Int\nrecurse(50)\nend")
            path.write_text("module Main\n" + body + "\n")
            for flag, value in (("--max-depth", "10"), ("--max-stack", "1KiB")):
                result = run(binary, ["run", flag, value, str(path)], status)
                assert not result.stdout
                if tail:
                    assert not result.stderr
                else:
                    assert flag.encode() in result.stderr and b"Main.recurse" in result.stderr
        path.write_text("module Main\nfn main()\nend\n")
        result = run(binary, ["run", "--max-heap", "1", str(path)], 70)
        assert not result.stdout and b"--max-heap = 1" in result.stderr

        for argument in (
            "if i % 2 == 0: 17 else: 29",
            "case i % 2\n0: 17\n_: 29\nend",
            "case i % 2\n0:\na = 17\na\n_:\nb = 29\nb\nend",
            "(fn()\na = if i % 2 == 0: 17 else: 29\na\nend)()",
        ):
            path.write_text(
                "module Main\nfn main(console: Console)\nvar xs = [7]\n"
                "for i in 0..3000\nxs = xs.append(" + argument + ")\nend\n"
                "for i in 0..3000\n"
                "if xs.get(i + 1) != Some(if i % 2 == 0: 17 else: 29)\n"
                'stop "wrong appended element"\nend\nend\n'
                'console.print("#{xs.size}:#{xs.get(0) or -1}")\nend\n')
            result = run(binary, ["run", str(path)], 0)
            assert result.stdout == b"3001:7\n" and not result.stderr

        # Loading/checking failure is never run as code. Per D250, a module
        # without main is a library module: check and test accept it, and E0079
        # comes only from run.
        path.write_text('module Main\nfn main() : Int\n"text"\nend\n')
        for command in ("check", "run"):
            result = run(binary, [command, str(path)], 1)
            assert not result.stdout and b"E0078" in result.stderr
        items = records(run(binary, ["check", "--json", str(path)], 1), "check")
        assert items[1]["code"] == "E0078" and items[-1]["dropped"] == 0

        path.write_text('module Main\nexpose identity\nfn identity(a: Int) : Int\n'
                        'examples\nidentity(5) == 5\nend\n'
                        'promise "keeps input"\nresult == a\n'
                        'allows a: 1, result: 1\nforbids a: 1, result: 2\nend\n'
                        'a\nend\n'
                        'test "library test item"\nassert identity(9) == 9\nend\n')
        for extra in ([], ["--emit=core"], ["--emit=bytecode"]):
            checked = run(binary, ["check", *extra, str(path)], 0)
            assert not checked.stderr
            if not extra:
                assert not checked.stdout
        records(run(binary, ["check", "--json", str(path)], 0), "check")
        tested = records(run(binary, ["test", "--json", str(path)], 0), "test")
        assert [(r["name"], r["outcome"]) for r in tested[1:-1]] == [
            ("Main.identity.example.1", "passed"),
            ("Main.identity.promise.1", "passed"),
            ("Main.test.1", "passed")]
        result = run(binary, ["run", str(path)], 1)
        assert not result.stdout and b"E0079" in result.stderr
        assert b"the entry module has no main function" in result.stderr

        service_image = path.parent / "service.bc"
        emitted = run(binary, ["emit", "services/task-queue/main.mo",
                               "-o", str(service_image)], 0)
        assert not emitted.stdout and not emitted.stderr
        assert service_image.read_bytes()[:4] == b"MOBC"

        for sample in ("examples/account.mo", "examples/job-queue.mo",
                       "examples/notes-tool.mo", "examples/rate-limit.mo"):
            result = subprocess.run([str(binary), "check", "--json", sample],
                                    cwd=root, capture_output=True, timeout=30)
            items = [json.loads(line) for line in result.stdout.splitlines()]
            assert all(r.get("code") != "E0079" for r in items), (sample, items)
        assert run(binary, ["check", "examples/account.mo"], 0).stderr == b""
        records(run(binary, ["check", "--json", "examples/account.mo"], 0), "check")
        assert b"E0079" in run(binary, ["run", "examples/account.mo"], 1).stderr
        account_tests = records(
            run(binary, ["test", "--json",
                         "--only", "Account.deposit.example.1",
                         "--only", "Account.deposit.example.2",
                         "--only", "Account.deposit.example.3",
                         "--only", "Account.deposit.example.4",
                         "--only", "Account.withdraw.example.1",
                         "--only", "Account.withdraw.example.2",
                         "--only", "Account.withdraw.example.3",
                         "--only", "Account.withdraw.example.4",
                         "--only", "Account.transfer.example.1",
                         "--only", "Account.transfer.example.2",
                         "--only", "Account.test.1",
                         "--only", "Account.test.2",
                         "examples/account.mo"], 0), "test")
        assert [(r["name"], r["outcome"]) for r in account_tests[1:-1]] == [
            ("Account.deposit.example.1", "passed"),
            ("Account.deposit.example.2", "passed"),
            ("Account.deposit.example.3", "passed"),
            ("Account.deposit.example.4", "passed"),
            ("Account.withdraw.example.1", "passed"),
            ("Account.withdraw.example.2", "passed"),
            ("Account.withdraw.example.3", "passed"),
            ("Account.withdraw.example.4", "passed"),
            ("Account.transfer.example.1", "passed"),
            ("Account.transfer.example.2", "passed"),
            ("Account.test.1", "passed"),
            ("Account.test.2", "passed")]
        assert account_tests[-1] == {"record": "totals", "errors": 0, "dropped": 0,
                                     "passed": 12, "failed": 0, "stopped": 0, "status": 0}
        assert run(binary, ["check", "examples/notes-tool.mo"], 0).stderr == b""
        records(run(binary, ["check", "--json", "examples/notes-tool.mo"], 0), "check")
        assert b"E0079" in run(binary, ["run", "examples/notes-tool.mo"], 1).stderr
        notes_tool_tests = records(
            run(binary, ["test", "--json", "--only", "NotesTool.test.1",
                         "--only", "NotesTool.test.2",
                         "--only", "NotesTool.test.3",
                         "examples/notes-tool.mo"], 0), "test")
        assert [(r["name"], r["outcome"]) for r in notes_tool_tests[1:-1]] == [
            ("NotesTool.test.1", "passed"),
            ("NotesTool.test.2", "passed"),
            ("NotesTool.test.3", "passed")]
        assert notes_tool_tests[-1] == {"record": "totals", "errors": 0, "dropped": 0,
                                        "passed": 3, "failed": 0, "stopped": 0, "status": 0}
        assert run(binary, ["check", "examples/rate-limit.mo"], 0).stderr == b""
        records(run(binary, ["check", "--json", "examples/rate-limit.mo"], 0), "check")
        assert b"E0079" in run(binary, ["run", "examples/rate-limit.mo"], 1).stderr
        rate_limit_tests = records(
            run(binary, ["test", "--json", "--only", "RateLimit.new_limiter.example.1",
                         "--only", "RateLimit.new_limiter.example.2",
                         "--only", "RateLimit.new_limiter.example.3",
                         "--only", "RateLimit.test.1",
                         "--only", "RateLimit.test.2",
                         "examples/rate-limit.mo"], 0), "test")
        assert [(r["name"], r["outcome"]) for r in rate_limit_tests[1:-1]] == [
            ("RateLimit.new_limiter.example.1", "passed"),
            ("RateLimit.new_limiter.example.2", "passed"),
            ("RateLimit.new_limiter.example.3", "passed"),
            ("RateLimit.test.1", "passed"),
            ("RateLimit.test.2", "passed")]
        assert rate_limit_tests[-1] == {"record": "totals", "errors": 0, "dropped": 0,
                                        "passed": 5, "failed": 0, "stopped": 0, "status": 0}
        assert run(binary, ["check", "examples/kv.mo"], 0).stderr == b""
        records(run(binary, ["check", "--json", "examples/kv.mo"], 0), "check")
        kv_run = subprocess.run([str(binary), "run", "examples/kv.mo"], cwd=root,
                                input=b"SET a 1\nINCR a\nGET a\nFLY\n",
                                capture_output=True, timeout=30)
        assert kv_run.returncode == 0 and kv_run.stderr == b""
        assert kv_run.stdout == b'OK\n(integer) 2\n"2"\n(error) unknown command FLY\n'
        kv_tests = records(
            run(binary, ["test", "--json",
                         "--only", "Kv.parse.example.1",
                         "--only", "Kv.parse.example.2",
                         "--only", "Kv.parse.example.3",
                         "--only", "Kv.parse.example.4",
                         "--only", "Kv.parse.example.5",
                         "--only", "Kv.parse.example.6",
                         "--only", "Kv.parse.example.7",
                         "--only", "Kv.increment.example.1",
                         "--only", "Kv.increment.example.2",
                         "--only", "Kv.increment.example.3",
                         "--only", "Kv.increment.example.4",
                         "--only", "Kv.expire.example.1",
                         "--only", "Kv.expire.example.2",
                         "--only", "Kv.expire.example.3",
                         "--only", "Kv.expire.example.4",
                         "--only", "Kv.test.1",
                         "--only", "Kv.test.2",
                         "examples/kv.mo"], 0), "test")
        assert [(r["name"], r["outcome"]) for r in kv_tests[1:-1]] == [
            ("Kv.parse.example.1", "passed"),
            ("Kv.parse.example.2", "passed"),
            ("Kv.parse.example.3", "passed"),
            ("Kv.parse.example.4", "passed"),
            ("Kv.parse.example.5", "passed"),
            ("Kv.parse.example.6", "passed"),
            ("Kv.parse.example.7", "passed"),
            ("Kv.increment.example.1", "passed"),
            ("Kv.increment.example.2", "passed"),
            ("Kv.increment.example.3", "passed"),
            ("Kv.increment.example.4", "passed"),
            ("Kv.expire.example.1", "passed"),
            ("Kv.expire.example.2", "passed"),
            ("Kv.expire.example.3", "passed"),
            ("Kv.expire.example.4", "passed"),
            ("Kv.test.1", "passed"),
            ("Kv.test.2", "passed")]
        assert kv_tests[-1] == {"record": "totals", "errors": 0, "dropped": 0,
                                "passed": 17, "failed": 0, "stopped": 0, "status": 0}
        assert run(binary, ["check", "examples/todo.mo"], 0).stderr == b""
        records(run(binary, ["check", "--json", "examples/todo.mo"], 0), "check")
        with tempfile.TemporaryDirectory() as folder:
            def todo(*words, status=0):
                result = subprocess.run([str(binary), "run", str(root / "examples/todo.mo"),
                                         *words], cwd=folder, capture_output=True, timeout=30)
                assert result.returncode == status, (words, result)
                return result
            for words in (["add", "milk"], ["add", "eggs"], ["done", "1"]):
                assert todo(*words).stdout == b""
            assert todo("list").stdout == b"1. [x] milk\n2. [ ] eggs\n"
            assert todo("remove", "9", status=1).stderr == b"there is no item 9\n"
            todo("remove", "2")
            assert (pathlib.Path(folder) / "todo.txt").read_text() == "[x] milk\n"
        # Files writes into a real folder: make_folder, append_text, remove, within, entries, walk.
        with tempfile.TemporaryDirectory() as folder:
            written = subprocess.run([str(binary), "run", str(root / "tests/cli/files-write/main.mo")],
                                     cwd=folder, capture_output=True, timeout=30)
            assert written.returncode == 0 and written.stderr == b"", written
            assert written.stdout == (b"true false\nentry a.txt\nentry logs\nwalk a.txt\nwalk logs\n"
                                      b"walk logs/old\nwalk logs/old/kept.txt\nwalk logs/run.txt\n"
                                      b"Error(out/a.txt: not a folder)\n"), written.stdout
            out = pathlib.Path(folder) / "out"
            assert sorted(str(p.relative_to(out)) for p in out.rglob("*")) == [
                "a.txt", "logs", "logs/old", "logs/old/kept.txt", "logs/run.txt"]
            assert (out / "a.txt").read_text() == "one\ntwo\n"
            assert (out / "logs/run.txt").read_text() == "start\n"
            assert (out / "logs/old/kept.txt").read_text() == "kept\n"
        # walk reports a link without entering it, and stops at a folder it can't read.
        with tempfile.TemporaryDirectory() as folder:
            tree = pathlib.Path(folder) / "tree"
            (tree / "b/locked").mkdir(parents=True)
            (tree / "a.txt").write_text("")
            (tree / "b/c.txt").write_text("")
            (tree / "link").symlink_to("b")

            def walk_tree():
                result = subprocess.run([str(binary), "run",
                                         str(root / "tests/cli/files-walk/main.mo")],
                                        cwd=folder, capture_output=True, timeout=30)
                assert result.returncode == 0 and result.stderr == b"", result
                return result.stdout
            assert walk_tree() == (b"a.txt file\nb folder\nb/c.txt file\nb/locked folder\n"
                                   b"link link\n")
            if os.geteuid() != 0:  # root reads a folder whatever its permissions
                (tree / "b/locked").chmod(0)
                try:
                    assert walk_tree() == b"error: tree/b/locked: not allowed\n"
                finally:
                    (tree / "b/locked").chmod(0o755)
        todo_tests = records(run(binary, ["test", "--json", "examples/todo.mo"], 0), "test")
        assert todo_tests[-1] == {"record": "totals", "errors": 0, "dropped": 0,
                                  "passed": 15, "failed": 0, "stopped": 0, "status": 0}
        assert run(binary, ["check", "examples/inventory.mo"], 0).stderr == b""
        records(run(binary, ["check", "--json", "examples/inventory.mo"], 0), "check")
        inventory_run = run(binary, ["run", "examples/inventory.mo"], 0)
        assert inventory_run.stderr == b""
        assert inventory_run.stdout == (
            b"The three most valuable stocks:\n"
            b"product    category  price  stock\n"
            b"Desk lamp  office    24.50     40\n"
            b"Notebook   office     3.20    300\n"
            b"Kettle     kitchen   39.90     12\n\n"
            b"kitchen: 2\noffice: 2\nhome: 1\n\n"
            b"One from each category: Kettle, Desk lamp, Doormat\n")
        inventory_tests = records(run(binary, ["test", "--json", "examples/inventory.mo"], 0),
                                  "test")
        assert inventory_tests[-1] == {"record": "totals", "errors": 0, "dropped": 0,
                                       "passed": 10, "failed": 0, "stopped": 0, "status": 0}
        assert run(binary, ["check", "examples/json-query.mo"], 0).stderr == b""
        records(run(binary, ["check", "--json", "examples/json-query.mo"], 0), "check")
        json_run = subprocess.run([str(binary), "run", "examples/json-query.mo", "items.0.n"],
                                  cwd=root, input=b'{"items": [{"n": -1.50}, null]}',
                                  capture_output=True, timeout=30)
        assert json_run.returncode == 0 and json_run.stderr == b""
        assert json_run.stdout == b"-1.5\n"
        json_tests = records(
            run(binary, ["test", "--json",
                         "--only", "JsonQuery.parse.example.1",
                         "--only", "JsonQuery.parse.example.2",
                         "--only", "JsonQuery.parse.example.3",
                         "--only", "JsonQuery.parse.example.4",
                         "--only", "JsonQuery.parse.example.5",
                         "--only", "JsonQuery.hex_value.example.1",
                         "--only", "JsonQuery.hex_value.example.2",
                         "--only", "JsonQuery.hex_value.example.3",
                         "--only", "JsonQuery.pretty.example.1",
                         "--only", "JsonQuery.pretty.example.2",
                         "--only", "JsonQuery.quoted.example.1",
                         "--only", "JsonQuery.quoted.example.2",
                         "--only", "JsonQuery.at_path.example.1",
                         "--only", "JsonQuery.at_path.example.2",
                         "--only", "JsonQuery.at_path.example.3",
                         "--only", "JsonQuery.test.1",
                         "examples/json-query.mo"], 0), "test")
        assert [(r["name"], r["outcome"]) for r in json_tests[1:-1]] == [
            ("JsonQuery.parse.example.1", "passed"),
            ("JsonQuery.parse.example.2", "passed"),
            ("JsonQuery.parse.example.3", "passed"),
            ("JsonQuery.parse.example.4", "passed"),
            ("JsonQuery.parse.example.5", "passed"),
            ("JsonQuery.hex_value.example.1", "passed"),
            ("JsonQuery.hex_value.example.2", "passed"),
            ("JsonQuery.hex_value.example.3", "passed"),
            ("JsonQuery.pretty.example.1", "passed"),
            ("JsonQuery.pretty.example.2", "passed"),
            ("JsonQuery.quoted.example.1", "passed"),
            ("JsonQuery.quoted.example.2", "passed"),
            ("JsonQuery.at_path.example.1", "passed"),
            ("JsonQuery.at_path.example.2", "passed"),
            ("JsonQuery.at_path.example.3", "passed"),
            ("JsonQuery.test.1", "passed")]
        assert json_tests[-1] == {"record": "totals", "errors": 0, "dropped": 0,
                                  "passed": 16, "failed": 0, "stopped": 0, "status": 0}
        assert run(binary, ["check", "examples/job-queue.mo"], 0).stderr == b""
        records(run(binary, ["check", "--json", "examples/job-queue.mo"], 0), "check")
        job_queue_tests = records(
            run(binary, ["test", "--json",
                         "--only", "JobQueue.lease.example.1",
                         "--only", "JobQueue.lease.example.2",
                         "--only", "JobQueue.fail.example.1",
                         "--only", "JobQueue.fail.example.2",
                         "--only", "JobQueue.fail.example.3",
                         "--only", "JobQueue.fail.example.4",
                         "--only", "JobQueue.expire.example.1",
                         "--only", "JobQueue.expire.example.2",
                         "--only", "JobQueue.test.1",
                         "--only", "JobQueue.test.2",
                         "--only", "JobQueue.test.3",
                         "--only", "JobQueue.test.4",
                         "examples/job-queue.mo"], 0), "test")
        assert [(r["name"], r["outcome"]) for r in job_queue_tests[1:-1]] == [
            ("JobQueue.lease.example.1", "passed"),
            ("JobQueue.lease.example.2", "passed"),
            ("JobQueue.fail.example.1", "passed"),
            ("JobQueue.fail.example.2", "passed"),
            ("JobQueue.fail.example.3", "passed"),
            ("JobQueue.fail.example.4", "passed"),
            ("JobQueue.expire.example.1", "passed"),
            ("JobQueue.expire.example.2", "passed"),
            ("JobQueue.test.1", "passed"),
            ("JobQueue.test.2", "passed"),
            ("JobQueue.test.3", "passed"),
            ("JobQueue.test.4", "passed")]
        assert job_queue_tests[-1] == {"record": "totals", "errors": 0, "dropped": 0,
                                       "passed": 12, "failed": 0, "stopped": 0, "status": 0}
        assert run(binary, ["check", "examples/expenses.mo"], 0).stderr == b""
        records(run(binary, ["check", "--json", "examples/expenses.mo"], 0), "check")
        with tempfile.TemporaryDirectory(prefix="mo-expenses-") as expenses_dir:
            pathlib.Path(expenses_dir, "spending.csv").write_text(
                "date,category,amount\n2026-01-01,rent,900\n2026-01-02,food,12.5\n"
                "2026-01-03,food,7.25\nbad\n")
            expenses_run = subprocess.run(
                [str(pathlib.Path(binary).resolve()), "run", str(root / "examples/expenses.mo"),
                 "spending.csv"], cwd=expenses_dir, capture_output=True, timeout=30)
        assert expenses_run.returncode == 1
        assert expenses_run.stderr == b"line 5: expected 3 fields, found 1\n"
        assert expenses_run.stdout == b"rent  900.00\nfood   19.75\n"
        expenses_names = [
            "Expenses.parse_line.example.1", "Expenses.parse_line.example.2",
            "Expenses.parse_line.example.3", "Expenses.parse_line.example.4",
            "Expenses.parse_line.example.5", "Expenses.totals.example.1",
            "Expenses.totals.example.2", "Expenses.layout.example.1", "Expenses.test.1"]
        expenses_tests = records(
            run(binary, ["test", "--json",
                         *[arg for name in expenses_names for arg in ("--only", name)],
                         "examples/expenses.mo"], 0), "test")
        assert [(r["name"], r["outcome"]) for r in expenses_tests[1:-1]] == [
            (name, "passed") for name in expenses_names]
        assert expenses_tests[-1] == {"record": "totals", "errors": 0, "dropped": 0,
                                      "passed": 9, "failed": 0, "stopped": 0, "status": 0}

        # Every source stage, including lower/Core/codegen, shares the explicit
        # source stack, while the caller has only 1 MiB. Nested arithmetic also
        # checks that the running entry, not an arbitrary helper, was selected.
        path.write_text("module Main\nfn main() : Int\n" + "1 + (" * 254
                        + "1" + ")" * 254 + "\nend\n")
        for args, status in ((["run", str(path)], 255),
                             (["check", "--emit=core", str(path)], 0),
                             (["check", "--emit=bytecode", str(path)], 0)):
            result = subprocess.run([str(binary), *args], cwd=root, capture_output=True,
                                    timeout=30, preexec_fn=small_stack)
            assert result.returncode == status and not result.stderr, (args, result)

        # Program output failing is a runtime bug, unlike guide's deliberate
        # silent broken-pipe exit. It must not become SIGPIPE or tools status 1.
        path.write_text('module Main\nfn main(console: Console)\nconsole.print("hello")\nend\n')
        for args, status, reason in ((["run", str(path)], 70, b"bug: "),
                                     (["check", "--emit=core", str(path)], 1, b"E1404")):
            read_fd, write_fd = os.pipe()
            os.close(read_fd)
            try:
                result = subprocess.run([str(binary), *args], cwd=root,
                                        stdout=write_fd, stderr=subprocess.PIPE, timeout=30)
                assert result.returncode == status and reason in result.stderr, result
            finally:
                os.close(write_fd)

        # The VM's structural report is terminal-safe too, including detail
        # bytes and paths. The escaped payload is not interpreted as control.
        unsafe = pathlib.Path(directory) / "bad\u009b\u202e"
        unsafe.mkdir()
        path = unsafe / "main.mo"
        path.write_text('module Main\nfn main()\nstop "\\u{1b}\\u{009b}\\u{202e} 日本語"\nend\n')
        result = run(binary, ["run", str(path)], 70, env={**os.environ, "LC_ALL": "C"})
        result.stderr.decode("ascii")
        assert b"\\u{1b}" in result.stderr and b"\\u{9b}" in result.stderr
        assert b"\\u{202e}" in result.stderr and b"\x1b" not in result.stderr
    print(f"{binary}: real source check/run/emission, statuses, limits and defects passed")
