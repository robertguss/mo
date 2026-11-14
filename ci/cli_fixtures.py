import errno
import os
import pathlib
import subprocess
import tempfile

from cli_common import records, root, run, utf8_locale
from cli_runner import small_stack


def lower_source_fixtures(binary):
    fixtures = root / "tests/lower-source"
    for name, expected in {
        "file-error-text": b"notes/a.md: no such file\n../secret: not allowed\nbin/app: not text\nout.txt: is a folder\nSome(notes/a.md: no such file)\n",
        "char-natives": b"[false, true, false, true, false, false]/[true, false, false, false, false, true]/[false, true, false]/true/66/b/Q\ntrue/false/true/false/[\"a\", \"b\"]/[(\"a\", 1), (\"b\", 2)]\n",
        "int-natives": b"81/-8/1/1\n-3/-3/2/-2\n"
                       b"-9223372036854775808/9223372036854775807/9223372036854775807/-9223372036854775808/-2/-9223372036854775808\n"
                       b"9223372036854775807/-9223372036854775808/-9223372036854775808/9223372036854775807/9223372036854775807/9223372036854775807\n"
                       b"None/Some(4)/None/Some(-21)\nSome(255)/None/None/Some(0)\n",
        "bigint-natives": b"1267650600228229401496703205376/-1267650600228229401496703205376/1267650600228229401496703205376\n"
                          b"-3/-3/2/-2\nNone/None/Some(9223372036854775807)/Some(-9223372036854775808)\n",
        "random-natives": b"",
        "float-natives": b"true/0/-0\n",
        "bytes-natives": b"10/0/Some(65)/Some(206)/Some(187)\nNone/None/None/Some(\"\")\n",
        "byte-natives": b"0/127/128/255/255\n",
        "builtin-abilities": ('Less/Equal/Greater\nEntry(name: "a\\n\\\"λ", kind: Folder)\n'
                              '{"b": Some(Less), "a": None}\n'
                              '{}/[(File, Link), (Other, Folder)]\nLess/Greater/Greater\n').encode(),
        "list-sum": b"7/7/-15/42\n0/0/0/0\n",
        "list-all": b"true/true/false/false/true/true/true\n",
        "list-find": b"none/2/none/2/bc/20/40\n",
        "list-count": b"0/0/3/2/2/2/3\n",
        "list-max": b"7/5/8/-2/zebra/b/3/6/30/9\n",
        "list-zip-entries": b'[("a", 1), ("b", 2)]/[]/[(1, "x"), (2, "y")]/[(true, "t"), (false, "f")]/[("b", 2), ("a", 1), ("c", 3)]\n',
        "list-map": b"[2, 4, 6]/[1, 2, 3]/[13, 14]/[0, 25]/[7, 8]\n",
        "option-or-fail": b"Ok(1)/Error(Missing(name: \"z\"))\nOk(42)/Error(Empty)\n"
                          b"Ok(3)/Error(Empty)\nOk(\"x\")/Error(7)\nmade eager\nOk(1)\n",
        "option-result-map": b"Some(20)/None\nSome(3)/Some(3)\nOk(20)/Error(Missing(name: \"z\"))\n"
                             b"Ok(1)/Error(\"no z\")\nOk(4)/Error(\"no q\")\nOk(true)\n"
                             b"None/Error(Missing(name: \"z\"))\nOk(1)\n",
        "json": "{\"a\":[1,2.5e1,\"\u00e9\"],\"b\":null}\nSome(1)/Some(25)/Some(25)/2.5e1\n"
                "Error(JsonError(line: 1, column: 5, problem: Unexpected(found: ']')))\n"
                "[\n  1,\n  {\n    \"k\": \"v\"\n  }\n]\n".encode(),
        "map-update-sort-desc": b"2/1,1/5,7/[3, 2, 1]/[\"abcd\", \"def\", \"bc\", \"a\"]/[\"top-a\", \"top-b\", \"mid-a\", \"mid-b\", \"first\", \"second\", \"third\"]/[5, 3, 2]\n",
        "map-literals": b"3/7/0/0/0/11\napples=3;pears=7;\n",
        "map-evaluation-order": b"key 2\nvalue 20\nkey 1\nvalue 10\n2=20\n1=10\nif effect\nwhile effect\n",
        "map-captures": b"3/4/19\n",
        "map-native-routes": b"11/3/2/12/12\n",
        "map-mutating-key": b"1=3;2=5;\n",
        "set": ('[3, 1, 2].to_set\n[].to_set\n'
                '["a\\n", "\\\"\\\\", "é"].to_set\n'
                "['\\n', '\\'', 'é'].to_set\n"
                '[[2, 1].to_set, [].to_set].to_set\n'
                'Some([3, 1, 2].to_set)\n[label 7, label 2].to_set\n').encode(),
        "time-sub": b"10000/-9750/5/-5000\n",
        "closure-captures": b"n10r2/12/13/15/6\n",
        "closure-returned": b"3/6/8/123/[2, 11, 102]/tick=8\n",
        "generic-higher-order": b'18/<<x>>/[1, 3, 0]/["1!", "2!"]/10/3,4,/true/false/("k", 7)\n',
        "generic-case": b'4/[1, 2, 5, 8]/["v1", "v2", "v5", "v8"]/["left 1", "right b"]/9/none\n',
        "generic-subject": b'("x", 7)/2.5/z\n',
    }.items():
        path = fixtures / name / "main.mo"
        checked = run(binary, ["check", str(path)], 0)
        assert not checked.stdout and not checked.stderr
        for form in ("core", "bytecode"):
            emitted = run(binary, ["check", f"--emit={form}", str(path)], 0)
            assert emitted.stdout and not emitted.stderr
        result = run(binary, ["run", str(path)], 0)
        assert result.stdout == expected and not result.stderr, (name, result)

    random_tests = records(run(binary, ["test", "--json",
                                       str(fixtures / "random-natives/main.mo")], 0), "test")
    assert random_tests[-1]["passed"] == 1 and random_tests[-1]["failed"] == 0
    assert random_tests[-1]["stopped"] == 0

    with tempfile.TemporaryDirectory(prefix="mo-numeric-bytes-") as directory:
        path = pathlib.Path(directory) / "main.mo"
        for expression, reason in (
            ("Int.pow(2, -1)", b"negative exponent"),
            ("2.pow(63)", b"overflow: Int"),
            ("Int.floor_div(-9223372036854775808, -1)", b"overflow: Int"),
            ("7.floor_div(0)", b"dividing by zero"),
            ("Int.floor_mod(7, 0)", b"dividing by zero"),
            ("[9223372036854775807, 1, -1].sum", b"overflow: Int"),
            ("[-9223372036854775808, -1, 1].sum", b"overflow: Int"),
            ("if [170141183460469231731.687303715884105727, 0.000000000000000001, -1.0].sum == 0.0: 0 else: 1",
             b"overflow: Decimal"),
        ):
            path.write_text(f"module Main\nfn main() : Int\n{expression}\nend\n")
            checked = run(binary, ["check", str(path)], 0)
            assert not checked.stdout and not checked.stderr
            result = run(binary, ["run", str(path)], 70)
            assert not result.stdout and reason in result.stderr, (expression, result)
        data = pathlib.Path(directory) / "input.bin"
        path.write_text('module Main\nfn main(files: Files) : Int\n'
                        'case files.read_bytes("input.bin")\n'
                        '  Ok(bytes): if bytes.decode_utf8 is None and '
                        'Bytes.decode_utf8(bytes) is None: 0 else: 1\n'
                        '  Error(_): 2\nend\nend\n')
        for payload in (b"\xff", b"\xc0\x80", b"\xed\xa0\x80", b"\xf0\x9f", b"a\x80"):
            data.write_bytes(payload)
            result = subprocess.run([str(binary.resolve()), "run", str(path)], cwd=directory,
                                    capture_output=True, timeout=30)
            assert result.returncode == 0 and not result.stdout and not result.stderr, result

    path = fixtures / "env-natives/main.mo"
    checked = run(binary, ["check", str(path)], 0)
    assert not checked.stdout and not checked.stderr
    result = run(binary, ["test", str(path)], 0)
    assert b"1 passed, 0 failed, 0 stopped" in result.stdout and not result.stderr
    with tempfile.TemporaryDirectory(prefix="mo-env-") as directory:
        path = pathlib.Path(directory) / "main.mo"
        path.write_text('module Main\nfn main(env: Env) : Int\n'
                        'if env.get("MO116_VALUE") == Some("λ") and '
                        'Env.get(env, "MO116_EMPTY") == Some("") and '
                        'env.get("MO116_ABSENT") is None: 0 else: 1\nend\n')
        environment = {**os.environ, "MO116_VALUE": "λ", "MO116_EMPTY": ""}
        environment.pop("MO116_ABSENT", None)
        result = run(binary, ["run", str(path)], 0, env=environment)
        assert not result.stdout and not result.stderr

    # Examples on generic subjects run once each, independently of promise instances.
    path = fixtures / "generic-subject/main.mo"
    for example in ("Main.swap.example.1", "Main.swap.example.2", "Main.larger.example.1",
                    "Main.larger.example.2"):
        result = run(binary, ["test", "--only", example, str(path)], 0)
        assert result.stdout.startswith(b"passed " + example.encode()), (example, result)
        assert b"1 passed, 0 failed, 0 stopped" in result.stdout and not result.stderr

    path = fixtures / "map-runtime-duplicate/main.mo"
    checked = run(binary, ["check", str(path)], 0)
    assert not checked.stdout and not checked.stderr
    result = run(binary, ["run", str(path)], 70)
    assert not result.stdout
    assert result.stderr.startswith(b"bug: two keys of a map literal that turn out equal")
    assert str(path).encode() in result.stderr and b"Main.duplicate" in result.stderr

    with tempfile.TemporaryDirectory(prefix="mo-map-literals-") as directory:
        path = pathlib.Path(directory) / "main.mo"
        for body in (
            'fn probe(key: Float) : Int\n{key: 3}.size\nend\n'
            'fn main() : Int\nprobe(1.0)\nend',
            'fn probe(key: fn(Int) : Int) : Int\n{key: 3}.size\nend\n'
            'fn main() : Int\nprobe(fn(n) n end)\nend',
            'fn main(console: Console) : Int\n{console: 3}.size\nend',
        ):
            path.write_text("module Main\n" + body + "\n")
            items = records(run(binary, ["check", "--json", str(path)], 1), "check")
            diagnostics = [r for r in items if r["record"] == "diagnostic"]
            assert diagnostics and all(r["code"] == "E0080" for r in diagnostics), items
            assert any("Hash" in r["headline"] for r in diagnostics), items
        for body in (
            'fn main() : Int\n{"apples": 1, "apples": 2}.size\nend',
            'fn main() : Int\nvar key = 1\n{key: 1, key: 2}.size\nend',
            'fn probe(key: Text) : Int\n{key: 1, key: 2}.size\nend\n'
            'fn main() : Int\nprobe("apples")\nend',
        ):
            path.write_text("module Main\n" + body + "\n")
            items = records(run(binary, ["check", "--json", str(path)], 1), "check")
            assert [r["code"] for r in items if r["record"] == "diagnostic"] == ["E0094"], items
    print(f"{binary}: map literal context, order, captures, abilities and duplicate keys passed")


def anchors(binary):
    # Anchor truth is not a typing certificate. Reordered arguments, opposite
    # expectations and a subject that stops expose distinct wrong consumers.
    source = ('module Main\nfn twice(n: Int) : Int\n'
              'promise "returns twice its input"\nresult == n + n\n'
              'allows result: 6, n: 3\nforbids n: 4, result: 7\nend\n'
              'if n >= 0: stop "subject must not run" else: n + n\nend\n'
              'fn main(console: Console) : Int\nconsole.print("main")\n37\nend\n')
    with tempfile.TemporaryDirectory(prefix="mo-anchors-") as directory:
        path = pathlib.Path(directory) / "main.mo"
        path.write_text(source)
        for args in (["check"], ["check", "--json"], ["check", "--emit=core"],
                     ["check", "--emit=bytecode"]):
            result = run(binary, [*args, str(path)], 0)
            assert not result.stderr
            if "--json" in args:
                records(result, "check")
            elif len(args) == 1:
                assert not result.stdout
            else:
                assert b"Main.twice.promise.1.allows.1" in result.stdout
        result = run(binary, ["run", str(path)], 37)
        assert result.stdout == b"main\n" and not result.stderr

        for allows, forbids in ((True, False), (False, True), (True, True)):
            bad = source.replace("result: 6", "result: 5") if allows else source
            if forbids:
                bad = bad.replace("result: 7", "result: 8")
            path.write_text(bad)
            items = records(run(binary, ["check", "--json", str(path)], 1), "check")
            diags = items[1:-1]
            assert len(diags) == allows + forbids
            assert all(d["code"] == "E1405" for d in diags)
            assert [d["primary"]["line"] for d in diags] == ([5] if allows else []) + ([6] if forbids else [])
            if allows:
                assert diags[0]["headline"] == "an allows anchor was rejected"
                assert diags[0]["secondary"][0]["line"] == 4
            if forbids:
                assert diags[-1]["headline"] == "a forbids anchor was accepted"
                assert not diags[-1]["secondary"]
            for args in (["check"], ["run"], ["check", "--emit=core"],
                         ["check", "--emit=bytecode"]):
                result = run(binary, [*args, str(path)], 1)
                assert not result.stdout and b"E1405" in result.stderr

        # A second false condition must retain its own source line, while a
        # rejected forbids may short-circuit before a later stop.
        path.write_text(source.replace("result == n + n\n", "result == n + n\nn < 2\n"))
        items = records(run(binary, ["check", "--json", str(path)], 1), "check")
        assert items[1]["primary"]["line"] == 6 and items[1]["secondary"][0]["line"] == 5
        path.write_text(source.replace("result == n + n\n",
                                       "result == n + n\nn == 3 or 1 / 0 == 0\n"))
        records(run(binary, ["check", "--json", str(path)], 0), "check")

        for operand, line, kind in (("n: 3", 5, "allows"), ("n: 4", 6, "forbids")):
            path.write_text(source.replace(operand, "n: 1 / 0"))
            items = records(run(binary, ["check", "--json", str(path)], 1), "check")
            assert items[1]["code"] == "E1405" and items[1]["primary"]["line"] == line
            assert items[1]["headline"] == "an anchor could not be checked"
            assert "dividing by zero" in items[1]["primary"]["label"]
            assert f"Main.twice.promise.1.{kind}.1" in items[1]["primary"]["label"]
            result = run(binary, ["run", str(path)], 1)
            assert not result.stdout and b"could not be checked" in result.stderr

        # Pure infinite computation is a compiler work refusal, not an accepted
        # anchor. The subject's and main's runtime semantics are unchanged.
        for operand in ("n: 3", "n: 4"):
            path.write_text(source.replace(operand, "n: spin()")
                            + "fn spin() : Int\nwhile true\nend\n0\nend\n")
            items = records(run(binary, ["check", "--json", str(path)], 1), "check")
            assert items[1]["headline"] == "an anchor could not be checked"
            assert "instruction budget exhausted" in items[1]["primary"]["label"]

        # This non-tail computation needs more depth than an anchor's own
        # tail call of its promise. A bound must stop certification before main.
        path.write_text(source.replace("n: 3", "n: descend(3)")
                        + "fn descend(n: Int) : Int\nif n == 0: 0 else: 1 + descend(n - 1)\nend\n")
        records(run(binary, ["check", "--json", str(path)], 0), "check")
        for flag in ("--max-stack", "--max-depth", "--max-heap"):
            result = run(binary, ["run", flag, "1", str(path)], 1)
            assert not result.stdout and b"E1405" in result.stderr
            assert b"could not be checked" in result.stderr

        # Both concrete promise instances must be certified. Ignoring the
        # Decimal instance or attaching its diagnostic to the Int anchor fails.
        generic = (source.replace("fn twice(n: Int) : Int\n",
                                  "fn twice(n: T) : T where T: Number, T: Equal, T: Order\n")
                   .replace("allows result: 6, n: 3\n",
                            "allows result: 6, n: 3\nallows n: 1.25, result: 2.5\n")
                   .replace("forbids n: 4, result: 7\n",
                            "forbids n: 4, result: 7\nforbids result: 2.0, n: 1.25\n"))
        path.write_text(generic)
        records(run(binary, ["check", "--json", str(path)], 0), "check")
        result = run(binary, ["run", str(path)], 37)
        assert result.stdout == b"main\n" and not result.stderr
        for kind in ("core", "bytecode"):
            result = run(binary, ["check", f"--emit={kind}", str(path)], 0)
            assert b"Main.twice(Decimal).promise.1" in result.stdout and not result.stderr

        path.write_text(generic.replace("result: 2.5", "result: 2.0")
                        .replace("result: 7", "result: 8"))
        items = records(run(binary, ["check", "--json", str(path)], 1), "check")
        assert [d["code"] for d in items[1:-1]] == ["E1405", "E1405"]
        assert [d["primary"]["line"] for d in items[1:-1]] == [6, 7]
        assert items[1]["secondary"][0]["line"] == 4 and not items[2]["secondary"]
        for args in (["run"], ["check", "--emit=core"], ["check", "--emit=bytecode"]):
            result = run(binary, [*args, str(path)], 1)
            assert not result.stdout and b"E1405" in result.stderr

        # Checking types examples/tests but does not test their truth or run main.
        path.write_text('module Main\nfn main()\nexamples\nfalse\nend\n'
                        'stop "main must not run"\nend\ntest "not run by check"\nassert false\nend\n')
        result = run(binary, ["check", str(path)], 0)
        assert not result.stdout and not result.stderr
        records(run(binary, ["check", "--json", str(path)], 0), "check")
    print(f"{binary}: real source anchor truth, stops, bounds and nonexecution passed")


def constants(binary):
    # Forward diamond dependencies and a generic call must initialize before
    # anchors read them. False examples/tests remain observations, not anchors.
    source = ('module Main\nanswer = twice(left) + right\nright = second()\n'
              'left = base + 2\nbase = 11\nfn second() : Int\nbase * 2\nend\n'
              'fn twice(n: T) : T where T: Number, T: Equal\n'
              'promise "doubles"\nresult == n + n\n'
              'allows result: 26, n: left\nforbids n: left, result: 25\nend\n'
              'examples\nfalse\nend\nn + n\nend\n'
              'fn main(console: Console) : Int\nconsole.print(answer.to_text)\nanswer\nend\n'
              'test "not run by check or run"\nassert false\nend\n')
    anchor = ('fn identity(n: Int) : Int\npromise "identity"\nresult == n\n'
              'allows n: 3, result: 3\nforbids result: 4, n: 3\nend\nn\nend\n')
    with tempfile.TemporaryDirectory(prefix="mo-constants-") as directory:
        path = pathlib.Path(directory) / "main.mo"
        path.write_text(source)
        result = run(binary, ["check", str(path)], 0)
        assert not result.stdout and not result.stderr
        records(run(binary, ["check", "--json", str(path)], 0), "check")
        result = run(binary, ["run", str(path)], 48)
        assert result.stdout == b"48\n" and not result.stderr
        for kind in ("core", "bytecode"):
            result = run(binary, ["check", f"--emit={kind}", str(path)], 0)
            assert result.stdout and not result.stderr
            emitted = pathlib.Path(directory) / f"constants.{kind}"
            emitted.write_bytes(result.stdout)
            result = run(binary.parent / "tests/test_cli.test",
                         [f"--roundtrip-{kind}", str(emitted)], 0)
            assert not result.stdout and not result.stderr
        path.write_text(source.replace("result: 26", "result: 27"))
        items = records(run(binary, ["check", "--json", str(path)], 1), "check")
        assert items[1]["code"] == "E1405" and items[-1]["errors"] == 1
        assert items[1]["primary"]["line"] == 12 and items[1]["secondary"][0]["line"] == 11
        result = run(binary, ["run", str(path)], 1)
        assert not result.stdout and b"an allows anchor was rejected" in result.stderr

        # Even unused constants initialize before main. Without anchors, check
        # and emission certify code but do not execute its initializers.
        main = ('fn main(console: Console) : Int\n'
                'console.print("main must not run")\n37\nend\n')
        for initializer, functions, reason in (
            ("1 / 0", "", "dividing by zero"),
            ("spin()", "fn spin() : Int\nwhile true\nend\n0\nend\n",
             "instruction budget exhausted"),
        ):
            base = "module Main\nunused = " + initializer + "\n" + functions + main
            path.write_text(base)
            result = run(binary, ["check", str(path)], 0)
            assert not result.stdout and not result.stderr
            records(run(binary, ["check", "--json", str(path)], 0), "check")
            for kind in ("core", "bytecode"):
                result = run(binary, ["check", f"--emit={kind}", str(path)], 0)
                assert result.stdout and not result.stderr
            if not functions:
                result = run(binary, ["run", str(path)], 70)
                assert not result.stdout and reason.encode() in result.stderr
                assert b"Main.unused.init" in result.stderr and b"Main.main" not in result.stderr

            # Initialization failure precedes both opposite expectations. It
            # is inability to check, never rejection that satisfies forbids.
            path.write_text(base + anchor)
            items = records(run(binary, ["check", "--json", str(path)], 1), "check")
            assert items[-1]["errors"] == 1 and items[-1]["dropped"] == 0
            diag = items[1]
            assert diag["code"] == "E1405" and diag["headline"] == "an anchor could not be checked"
            assert diag["primary"]["file"] is None and not diag["secondary"]
            assert reason in diag["primary"]["label"] and "Main.unused.init" in diag["primary"]["label"]
            for args in (["run"], ["check", "--emit=core"], ["check", "--emit=bytecode"]):
                result = run(binary, [*args, str(path)], 1)
                assert not result.stdout and b"E1405" in result.stderr
                assert reason.encode() in result.stderr and b"Main.main" not in result.stderr

        # Calling a constant function value is not another read of the value.
        # A permitted closure must not be rejected with deferred globals.
        for body, status in (
            ('base = 13\nhandler = first\nanswer = handler()\n'
             'reader = fn() Some(try Some(1)) end\nfn first() : Int\nbase\nend\n'
             'fn main() : Int\nanswer + (reader() or 9)\nend\n', 14),
            ('base = 13\nhandler = fn() base end\n'
             'fn main() : Int\nhandler() + 24\nend\n', 37),
            ('answer = handler(-7)\nhandler = Int.abs\n'
             'fn main() : Int\nanswer + 30\nend\n', 37),
            ('answer = handler(2, 3)\nhandler = Int.pow\n'
             'fn main() : Int\nanswer + 30\nend\n', 38),
            ('fn main() : Int\nInt.min(7, -11) * 2 + Int.max(-11, 7) + 68\nend\n', 53),
            ('to_i = Decimal.to_int\n'
             'fn main() : Int\n'
             'a = (-2.4).abs.max(Decimal.min(3.1, 2.7)).ceil + 1.9.floor + 2.345.round(2)\n'
             'f = Decimal.to_fixed\n'
             'fl = Decimal.to_float\n'
             'if f(a, 2) == "6.35" and fl(0.5) == 0.5\n'
             '  to_i(3.0.max(1.0)) or 0\n'
             'else\n'
             '  0\n'
             'end\nend\n', 3),
            ('trim_fn = Text.trim\n'
             'join_fn = List.join\n'
             'fn main() : Int\n'
             'take_fn = List.take\n'
             'parts = take_fn("  a.md,b.md,c.txt  ".trim.split(","), 2)\n'
             'joined = join_fn(parts, ":")\n'
             'if joined.ends_with?(".md") and not trim_fn(" x ").empty?\n'
             '  "42".to_int or 0\n'
             'else\n'
             '  0\n'
             'end\nend\n', 42),
        ):
            path.write_text("module Main\n" + body)
            result = run(binary, ["check", str(path)], 0)
            assert not result.stdout and not result.stderr
            records(run(binary, ["check", "--json", str(path)], 0), "check")
            result = run(binary, ["run", str(path)], status)
            assert not result.stdout and not result.stderr
            for kind in ("core", "bytecode"):
                result = run(binary, ["check", f"--emit={kind}", str(path)], 0)
                assert result.stdout and not result.stderr
                emitted = pathlib.Path(directory) / f"constant-functions.{kind}"
                emitted.write_bytes(result.stdout)
                result = run(binary.parent / "tests/test_cli.test",
                             [f"--roundtrip-{kind}", str(emitted)], 0)
                assert not result.stdout and not result.stderr

        # A lazy body is not an initialization dependency. Neither a lambda,
        # a named adapter nor a self-alias may access beyond its creation prefix.
        for body, reason in (
            ('handler = fn() base end\nbase = 13\n'
             'fn main() : Int\nhandler() + 24\nend\n', "deferred function"),
            ('answer = handler()\nhandler = first\nbase = 13\n'
             'fn first() : Int\nbase\nend\nfn main() : Int\nanswer + 24\nend\n',
             "deferred function"),
            ('handler = first\nfn first() : Int\nhandler()\nend\n'
             'fn main() : Int\n17\nend\n', "deferred function"),
        ):
            path.write_text("module Main\n" + body)
            items = records(run(binary, ["check", "--json", str(path)], 1), "check")
            assert items[-1]["errors"] == 1 and items[-1]["dropped"] == 0
            assert items[1]["code"] == "E1150" and reason in items[1]["headline"]
            for args in (["check"], ["run"], ["check", "--emit=core"],
                         ["check", "--emit=bytecode"]):
                result = run(binary, [*args, str(path)], 1)
                assert not result.stdout and b"E1150" in result.stderr
                assert reason.encode() in result.stderr and b"invalid compiler Core" not in result.stderr
    print(f"{binary}: source constant initialization, generic anchors and refusal boundaries passed")


def filenames(binary):
    # The operating system accepts byte paths. Do not repair them into Text or
    # confuse filename indices with ordinary string-table indices.
    with tempfile.TemporaryDirectory(prefix="mo-filenames-") as directory:
        reports = []
        for raw, escaped in ((b"\xff", b"\\xff"), (b"\xc3", b"\\xc3"),
                             ("ÿ".encode(), "ÿ".encode()),
                             (b"\\xff", b"\\\\xff"), (b"line\n\\", b"line\\n\\\\")):
            folder = os.fsencode(directory) + b"/" + raw
            try:
                os.mkdir(folder)
            except OSError as error:
                # APFS/HFS+ refuse non-UTF-8 names; don't hide unrelated I/O errors.
                if error.errno != errno.EILSEQ:
                    raise
                print(f"{binary}: filesystem refuses the name {escaped.decode()}; skipped")
                continue
            path = folder + b"/main.mo"
            with open(path, "wb") as file:
                file.write(b"module Main\nfn main() : Int\n37\nend\n")
            result = run(binary, ["check", path], 0)
            assert not result.stdout and not result.stderr
            result = run(binary, ["run", path], 37)
            assert not result.stdout and not result.stderr
            result = run(binary, ["check", "--emit=bytecode", path], 0)
            assert not result.stderr and path.hex().encode() in result.stdout
            emitted = pathlib.Path(directory) / "program.bytecode"
            emitted.write_bytes(result.stdout)
            run(binary.parent / "tests/test_cli.test", ["--roundtrip-bytecode", str(emitted)], 0)

            with open(path, "wb") as file:
                file.write(b'module Main\nfn main()\nstop "chosen stop"\nend\n')
            result = run(binary, ["run", path], 70,
                         env={**os.environ, "LC_ALL": utf8_locale})
            result.stderr.decode("utf-8")
            assert not result.stdout and b"chosen stop" in result.stderr
            assert b"/" + escaped + b"/main.mo:" in result.stderr
            reports.append(result.stderr)

            # Anchor diagnostics must find both locations in the retained
            # source store after codegen/load, even for invalid UTF-8 paths.
            source = (b'module Main\nfn f(n: Int) : Int\npromise "identity"\n'
                      b'result == n\nallows n: 3, result: 4\nforbids n: 3, result: 2\n'
                      b'end\nn\nend\nfn main()\nend\n')
            with open(path, "wb") as file:
                file.write(source)
            items = records(run(binary, ["check", "--json", path], 1), "check")
            diag = items[1]
            assert diag["code"] == "E1405" and diag["primary"]["line"] == 5
            assert diag["primary"]["file"].endswith("/main.mo")
            assert diag["primary"]["file_bytes"] == path.hex()
            assert diag["primary"]["start"] == source.index(b"allows")
            assert diag["secondary"][0]["line"] == 4
            assert diag["secondary"][0]["file_bytes"] == path.hex()
            assert diag["secondary"][0]["start"] == source.index(b"result == n")
        assert len(set(reports)) == len(reports)
    print(f"{binary}: exact filename-byte execution, bytecode and anchor locations passed")


def source_stack(binary, folder):
    # This fixture exercises the entire precedence ladder at the source-depth
    # boundary, not only nested brackets. The process stack stays small while
    # loading, parsing and whole checking use the explicit source-work stack.
    path = folder / "main.mo"
    header = ("module Main\nfn run(a: Bool, b: Bool, c: Bool, d: Range, e: Int, "
              "f: Int, g: Int, h: fn(Bool) : Int) : Bool\n")

    for depth, status in ((255, 0), (256, 1)):
        path.write_text(header + "a implies b or c and not d == e .. f + g * -h(" * depth
                        + "a" + ")" * depth + "\nend\nfn main()\nend\n")
        for json_mode in (False, True):
            result = subprocess.run(
                [str(binary), "check", *(["--json"] if json_mode else []), str(path)],
                cwd=root, capture_output=True, timeout=30, preexec_fn=small_stack)
            assert result.returncode == status, (depth, json_mode, result.returncode, result.stderr)
            if json_mode:
                items = records(result, "check")
                assert items[-1]["errors"] == status and items[-1]["dropped"] == 0
                if status:
                    assert items[1]["code"] == "E1006"
            else:
                assert not result.stdout
                assert (b"E1006" in result.stderr) if status else not result.stderr
