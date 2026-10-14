#!/usr/bin/env python3
"""Black-box CLI contracts and goldens, with Python's real JSON parser."""
import errno
import json
import locale
import os
import pathlib
import resource
import stat
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parent.parent

# Select an installed UTF-8 locale for the raw-text regression, independently
# of the CI shell's locale. C/POSIX/unavailable locales are tested separately.
for utf8_locale in ("C.UTF-8", "en_US.UTF-8", "UTF-8"):
    try:
        locale.setlocale(locale.LC_CTYPE, utf8_locale)
        break
    except locale.Error:
        pass
else:
    raise AssertionError("CLI display tests need an installed UTF-8 locale")


def run(binary, args, status, env=None):
    result = subprocess.run([str(binary), *args], cwd=root, env=env, capture_output=True, timeout=30)
    assert result.returncode == status, (args, result.returncode, result.stderr)
    return result


def records(result, command):
    assert not result.stderr, result.stderr
    lines = result.stdout.decode("utf-8").splitlines()
    assert result.stdout.endswith(b"\n")
    items = [json.loads(line) for line in lines]
    assert items[0] == {"record": "version", "format": "1.0", "mo": "0.0.0", "command": command}
    assert items[-1]["record"] == "totals"
    assert items[-1]["status"] == result.returncode
    assert items[-1]["errors"] == sum(item["record"] == "diagnostic" for item in items)
    return items


def scalar_promises(binary):
    with tempfile.TemporaryDirectory(prefix="mo-scalar-promises-") as directory:
        path = pathlib.Path(directory) / "main.mo"
        source = ('module Main\nfn keep(value: Int) : Int\n'
                  'promise "keeps its input"\nresult == value\n'
                  'allows value: 3, result: 3\nforbids value: 3, result: 4\nend\n'
                  'value\nend\ntest "after promise"\nassert true\nend\n'
                  'fn main()\nstop "main ran"\nend\n')
        path.write_text(source)
        items = records(run(binary, ["test", "--json", str(path)], 0), "test")
        assert [(r["name"], r["outcome"]) for r in items[1:-1]] == [
            ("Main.keep.promise.1", "passed"), ("Main.test.1", "passed")]
        assert items[-1] == {"record": "totals", "errors": 0, "dropped": 0,
                             "passed": 2, "failed": 0, "stopped": 0, "status": 0}


def observations(binary):
    with tempfile.TemporaryDirectory(prefix="mo-observations-") as directory:
        path = pathlib.Path(directory) / "main.mo"
        path.write_text('module Main\nseed = abort()\nfn abort() : Int\n'
                        'stop "initialization ran"\nend\ntest "not run"\n'
                        'assert false\nend\nfn main()\nstop "main ran"\nend\n')
        args = ["test", "--list", "--max-stack", "1", "--max-depth", "1",
                "--max-heap", "1", "--calls", "1", "--seconds", "1", str(path)]
        text = run(binary, args, 0)
        assert text.stdout == b"Main.test.1\n" and not text.stderr
        items = records(run(binary, [*args, "--json"], 0), "test")
        assert len(items) == 3 and items[1] == {
            "record": "test", "name": "Main.test.1", "kind": "test",
            "span": {"file": str(path), "start": 74, "end": 105, "line": 6,
                     "column": 1, "end_line": 8, "end_column": 4}}
        assert items[-1] == {"record": "totals", "errors": 0, "dropped": 0,
                             "passed": 0, "failed": 0, "stopped": 0, "status": 0}
        for selectors in (["--only", "Main.test.1", "--only", "Main.test.1", "--in", "Main"],
                          ["--only", "Main.test.1"]):
            result = run(binary, ["test", "--list", *selectors, str(path)], 0)
            assert result.stdout == b"Main.test.1\n" and not result.stderr
        for selectors in (["--only", "Main.test"], ["--only", "Main.test.10"],
                          ["--in", "Mai"], ["--in", "Main.Child"],
                          ["--only", "Main.test.1", "--only", "Typo.test.1"]):
            result = run(binary, ["test", "--list", "--json", *selectors, str(path)], 2)
            assert not result.stdout and result.stderr.count(b"\n") == 1
        initialized = records(run(binary, ["test", "--json", str(path)], 1), "test")
        assert initialized[1]["code"] == "E1406"
        assert "initialization ran" in initialized[1]["primary"]["label"]
        assert all(initialized[-1][key] == 0 for key in ("passed", "failed", "stopped"))

        source = ('module Main\nbase = 13\nfn plus(n: Int) : Int\nexamples\n'
                  'plus(3) == 16\nend\nexamples\nplus(8) == 99\nend\nn + base\nend\n'
                  'test "second assert"\nassert true\n'
                  'assert "λ\\u{0}right 2:X,\\n" == "日本"\nend\n'
                  'test "after failure"\nassert base == 13\nend\n'
                  'fn main()\nstop "main ran"\nend\n')
        path.write_text(source)
        items = records(run(binary, ["test", "--json", str(path)], 1), "test")
        expected = [("Main.plus.example.1", "example", "passed"),
                    ("Main.plus.example.2", "example", "failed"),
                    ("Main.test.1", "test", "failed"), ("Main.test.2", "test", "passed")]
        assert [(r["name"], r["kind"], r["outcome"]) for r in items[1:-1]] == expected
        assert all(type(r["microseconds"]) is int and r["microseconds"] >= 0 for r in items[1:-1])
        assert items[2]["left"] == "21" and items[2]["right"] == "99"
        assert items[3]["left"] == "λ\0right 2:X,\n" and items[3]["right"] == "日本"
        assert items[3]["line"] == {"file": str(path), "start": 144, "end": 184,
                                    "line": 14, "column": 1, "end_line": 14, "end_column": 36}
        assert items[-1] == {"record": "totals", "errors": 0, "dropped": 0,
                             "passed": 2, "failed": 2, "stopped": 0, "status": 1}
        text = run(binary, ["test", str(path)], 1)
        assert not text.stderr and text.stdout.endswith(b"2 passed, 2 failed, 0 stopped\n")
        assert b"this example was false" in text.stdout and b"this assertion was false" in text.stdout
        assert "λ\\x00right 2:X,\\x0a".encode() in text.stdout and b"main ran" not in text.stdout
        selected = records(run(binary, ["test", "--json", "--only", "Main.test.2",
                                        "--only", "Main.plus.example.1", "--only", "Main.test.2",
                                        "--in", "Main", str(path)], 0), "test")
        assert [r["name"] for r in selected[1:-1]] == ["Main.plus.example.1", "Main.test.2"]
        assert selected[-1]["passed"] == 2 and selected[-1]["failed"] == selected[-1]["stopped"] == 0

        # A generic promise with no ordinary instantiation must not disappear
        # from either listing or unsupported-selection preflight.
        path.write_text('module Main\nfn keep(value: T) : T where T: Equal\n'
                        'promise "keeps input"\nresult == value\n'
                        'allows value: 3, result: 3\nforbids value: 3, result: 4\nend\n'
                        'value\nend\ntest "selected control"\nassert true\nend\n'
                        'fn main()\nstop "main ran"\nend\n')
        listed = records(run(binary, ["test", "--json", "--list", str(path)], 0), "test")
        assert [r["name"] for r in listed[1:-1]] == ["Main.keep.promise.1", "Main.test.1"]
        original = path.read_text()
        path.write_text(original.replace('allows value: 3, result: 3', 'allows value: 3, result: 4'))
        listed = records(run(binary, ["test", "--json", "--list", str(path)], 0), "test")
        assert [r["name"] for r in listed[1:-1]] == ["Main.keep.promise.1", "Main.test.1"]
        anchored = records(run(binary, ["test", "--json", "--only", "Main.test.1", str(path)], 1), "test")
        assert anchored[1]["code"] == "E1405" and anchored[-1]["passed"] == 0
        path.write_text(original)
        refused = records(run(binary, ["test", "--json", str(path)], 1), "test")
        assert len(refused) == 3 and refused[1]["code"] == "E1400"
        assert refused[1]["primary"]["label"] == "Main.keep.promise.1"
        assert all(refused[-1][key] == 0 for key in ("passed", "failed", "stopped"))
        accepted = records(run(binary, ["test", "--json", "--only", "Main.test.1", str(path)], 0), "test")
        assert len(accepted) == 3 and accepted[1]["outcome"] == "passed"
        # The same promise selection must refuse before even an unused stopping
        # initializer, and before an ordinary test that occurs first in source.
        source = path.read_text().replace('test "selected control"\nassert true\nend\n', '')
        path.write_text(source.replace('module Main\n', 'module Main\nseed = crash()\n'
                                       'fn crash() : Int\nstop "must not initialize"\nend\n'
                                       'test "selected control"\nassert true\nend\n'))
        refused = records(run(binary, ["test", "--json", "--only", "Main.test.1",
                                       "--only", "Main.keep.promise.1", str(path)], 1), "test")
        assert len(refused) == 3 and refused[1]["code"] == "E1400"
        assert all(refused[-1][key] == 0 for key in ("passed", "failed", "stopped"))

        source = ('module Main\ntest "Bool, not equality"\nassert (\n  false\n)\nend\n'
                  'test "after Bool failure"\nassert true\nend\nfn main()\nend\n')
        path.write_text(source)
        items = records(run(binary, ["test", "--json", str(path)], 1), "test")
        assert items[1]["outcome"] == "failed" and items[2]["outcome"] == "passed"
        assert "left" not in items[1] and "right" not in items[1]
        start = source.index("assert (")
        assert items[1]["line"] == {"file": str(path), "start": start,
                                    "end": start + len("assert (\n  false\n)"),
                                    "line": 3, "column": 1, "end_line": 5, "end_column": 2}

        # Structural failure printing must not call the source Text customizer.
        path.write_text('module Main\nstruct Box\nvalue: Int\nend\n'
                        'fn to_text(_value: Box) : Text\nstop "renderer called"\nend\n'
                        'fn same(value: Box) : Box\nexamples\nsame(Box(value: 7)) == Box(value: 9)\nend\n'
                        'value\nend\nfn main()\nend\n')
        items = records(run(binary, ["test", "--json", str(path)], 1), "test")
        assert items[1]["outcome"] == "failed"
        assert (items[1]["left"], items[1]["right"]) == ("Box(value: 7)", "Box(value: 9)")
        assert items[-1]["failed"] == 1 and items[-1]["stopped"] == 0

        # Record budgets reject a whole record, never truncated values or a
        # fabricated passing observation. Counts retain the attempted failure.
        for count, fits in ((17, True), (18, False)):
            path.write_text('module Main\ntest "large structural side"\nvar value = "\\u{1}"\n'
                            f'var n = 0\nwhile n < {count}\nvalue = "#{{value}}#{{value}}"\n'
                            'n = n + 1\nend\nassert value == ""\nend\n'
                            'test "after output"\nassert true\nend\nfn main()\nend\n')
            items = records(run(binary, ["test", "--json", str(path)], 1), "test")
            if fits:
                assert items[1]["left"] == "\1" * (1 << count) and items[1]["right"] == ""
                assert items[-1]["passed"] == 1 and items[-1]["errors"] == 0
            else:
                assert len(items) == 3 and items[1]["code"] == "E1404"
                assert items[-1]["passed"] == 0 and items[-1]["errors"] == 1
            assert items[-1]["failed"] == 1 and items[-1]["stopped"] == 0

        # A bug ends only its fresh process. main is never the test entry, and
        # neither a stop nor a failure may hide the following successful test.
        source = ('module Main\nfn divide(n: Int) : Int\nexamples\ndivide(7) == 0\nend\n'
                  'n / 0\nend\ntest "after stop"\nassert true\nend\n'
                  'test "failed after stop"\nassert 17 == 29\nend\n'
                  'fn main()\nstop "main must not run"\nend\n')
        path.write_text(source)
        items = records(run(binary, ["test", "--json", str(path)], 1), "test")
        assert [r["outcome"] for r in items[1:-1]] == ["stopped", "passed", "failed"]
        defect = items[1]["defect"]
        start = source.index("n / 0")
        assert defect["reason"] == "dividing by zero"
        assert defect["span"] == {"file": str(path), "start": start, "end": start + 5,
                                   "line": 6, "column": 1, "end_line": 6, "end_column": 6}
        assert [f["function"] for f in defect["stack"]] == ["Main.divide", "Main.divide.example.1"]
        assert defect["variables"] == [{"name": "n", "value": "7"}]
        assert items[-1] == {"record": "totals", "errors": 0, "dropped": 0,
                             "passed": 1, "failed": 1, "stopped": 1, "status": 1}
        text = run(binary, ["test", str(path)], 1)
        assert not text.stderr and b"bug: dividing by zero" in text.stdout
        assert b"n = 7" in text.stdout and b"main must not run" not in text.stdout
        assert text.stdout.endswith(b"1 passed, 1 failed, 1 stopped\n")

        # Depth/stack exhaustion is a stopped result, not a failed assertion or
        # source diagnostic. A succeeding process follows under the same bound.
        path.write_text('module Main\nfn descend(n: Int) : Int\n'
                        'if n == 0: 0 else: 1 + descend(n - 1)\nend\n'
                        'test "limited"\nassert descend(30) == 30\nend\n'
                        'test "fresh process"\nassert true\nend\nfn main()\nend\n')
        for flag, value in (("--max-depth", "10"), ("--max-stack", "1KiB")):
            items = records(run(binary, ["test", "--json", flag, value, str(path)], 1), "test")
            assert [r["outcome"] for r in items[1:-1]] == ["stopped", "passed"]
            assert items[1]["defect"]["reason"] == "out of stack"
            assert items[1]["defect"]["limit"] == {"name": flag, "value": "10" if value == "10" else "1024"}
            assert all(f["span"]["file"] == str(path) for f in items[1]["defect"]["stack"])
            assert items[-1]["passed"] == items[-1]["stopped"] == 1 and items[-1]["errors"] == 0

        # Original stop payloads retain NUL/newline, while both encodings remain
        # terminal-safe. Native scalar overflow uses the same stopped route.
        path.write_text('module Main\nhandler = Int.abs\ntest "native overflow"\n'
                        'assert handler(-9223372036854775808) == 0\nend\n'
                        'test "explicit stop"\nstop "λ\\u{0}\\n\\u{1b}"\nend\n'
                        'test "after both"\nassert true\nend\nfn main()\nend\n')
        items = records(run(binary, ["test", "--json", str(path)], 1), "test")
        assert [r["outcome"] for r in items[1:-1]] == ["stopped", "stopped", "passed"]
        assert items[1]["defect"]["reason"] == "overflow: Int"
        assert items[2]["defect"]["reason"] == "stop" and items[2]["defect"]["detail"] == "λ\0\n\x1b"
        assert items[-1]["stopped"] == 2 and items[-1]["errors"] == 0
        text = run(binary, ["test", str(path)], 1, env={**os.environ, "LC_ALL": "C"})
        text.stdout.decode("ascii")
        assert not text.stderr and b"\\x00\\x0a\\x1b" in text.stdout and b"\x1b" not in text.stdout

        # Values not yet read remain reportable; the unassigned n does not.
        # Quoting is structural, including interpolation and quote escapes,
        # and cannot execute the source Box Text implementation.
        path.write_text(r'''module Main
struct Box
value: Int
end
fn to_text(_box: Box) : Text
stop "renderer called"
end
fn crash(label: Text, mark: Char, box: Box) : Int
n = 7 / 0
n + label.byte_count + mark.to_text.byte_count + box.value
end
test "quoted variables"
outer = 11
assert crash("λ\u{0}\n\"\#{0}", '\'', Box(value: 7)) + outer == 0
end
fn main()
end
''')
        items = records(run(binary, ["test", "--json", str(path)], 1), "test")
        assert items[1]["defect"]["variables"] == [
            {"name": "label", "value": r'"λ\u{0}\n\"\#{0}"'},
            {"name": "mark", "value": r"'\''"},
            {"name": "box", "value": "Box(value: 7)"}]
        assert items[1]["defect"]["reason"] == "dividing by zero" and items[-1]["errors"] == 0
        for length in (4094, 4095):
            path.write_text('module Main\nfn crash(label: Text) : Int\nn = 7 / 0\n'
                            'n + label.byte_count\nend\ntest "print budget"\n'
                            'assert crash("' + 'x' * length + '") == 0\nend\nfn main()\nend\n')
            items = records(run(binary, ["test", "--json", str(path)], 1), "test")
            expected = '"' + 'x' * length + '"' if length == 4094 else "<value unavailable>"
            assert items[1]["defect"]["variables"] == [{"name": "label", "value": expected}]
            assert items[-1]["stopped"] == 1 and items[-1]["errors"] == 0

        # Compiler certification's instruction ceiling is not an ordinary-test
        # deadline, and --calls/--seconds are promise-only budgets.
        path.write_text('module Main\ntest "long ordinary computation"\nvar n = 0\n'
                        'while n < 5000001\nn += 1\nend\nassert n == 5000001\nend\nfn main()\nend\n')
        items = records(run(binary, ["test", "--json", "--calls", "1", "--seconds", "1", str(path)], 0), "test")
        assert items[1]["outcome"] == "passed" and items[-1]["passed"] == 1

        # Exact owning module, not a prefix of a nested module's name.
        nested = pathlib.Path(directory) / "main" / "child.mo"
        nested.parent.mkdir()
        nested.write_text('module Main.Child\ntest "nested module"\nassert true\nend\nfn main()\nend\n')
        assert run(binary, ["test", "--list", "--in", "Main.Child", str(nested)], 0).stdout == b"Main.Child.test.1\n"
        result = run(binary, ["test", "--list", "--json", "--in", "Main", str(nested)], 2)
        assert not result.stdout and result.stderr.count(b"\n") == 1
    print(f"{binary}: source inventory, exact selections and list nonexecution passed")


def captures(binary):
    # Each returned closure outlives its creating frames. A provenance symbol
    # must not turn the outer invocation back into a direct factory call.
    source = ('module Main\nfn make(left: Int, right: Int) : fn(Int) : fn() : Int\n'
              'held = right + 5\nfn(extra)\nfn() left * 7 + held + extra end\nend\nend\n'
              'fn divide(base: Int) : fn(Int) : Int\nfn(value) base / value end\nend\n'
              'test "escaped frames"\nassert make(3, 29)(5)() == 60\nend\n'
              'test "explicit snapshot"\nvar count = 11\ncopy = count\n'
              'show = fn() copy * 3 end\ncount += 5\nassert show() * 10 + count == 346\nend\n'
              'test "wrong escaped answer"\naction = make(3, 29)(5)\nassert action() == 61\nend\n'
              'test "escaped stop"\naction = divide(17)\nassert action(0) == 1\nend\n'
              'test "after stop"\nassert make(5, 11)(2)() == 53\nend\n'
              'fn main() : Int\nstop "main must not execute"\nend\n')
    with tempfile.TemporaryDirectory(prefix="mo-cli-captures-") as directory:
        path = pathlib.Path(directory) / "main.mo"
        path.write_text(source)
        checked = run(binary, ["check", str(path)], 0)
        assert not checked.stdout and not checked.stderr
        items = records(run(binary, ["test", "--json", str(path)], 1), "test")
        assert [r["name"] for r in items[1:-1]] == [f"Main.test.{n}" for n in range(1, 6)]
        assert [r["outcome"] for r in items[1:-1]] == ["passed", "passed", "failed", "stopped", "passed"]
        assert (items[3]["left"], items[3]["right"]) == ("60", "61")
        start = source.index("assert action()")
        assert items[3]["line"] == {"file": str(path), "start": start,
                                    "end": start + len("assert action() == 61"),
                                    "line": 23, "column": 1, "end_line": 23, "end_column": 22}
        start = source.index("base / value")
        defect = items[4]["defect"]
        assert defect["reason"] == "dividing by zero"
        assert defect["span"] == {"file": str(path), "start": start,
                                   "end": start + len("base / value"),
                                   "line": 9, "column": 11, "end_line": 9, "end_column": 23}
        assert [f["function"] for f in defect["stack"]] == ["Main.divide.fn.1", "Main.test.4"]
        assert defect["variables"] == [{"name": "base", "value": "17"}, {"name": "value", "value": "0"}]
        assert items[-1] == {"record": "totals", "errors": 0, "dropped": 0,
                             "passed": 3, "failed": 1, "stopped": 1, "status": 1}
        text = run(binary, ["test", str(path)], 1)
        assert not text.stderr and text.stdout.endswith(b"3 passed, 1 failed, 1 stopped\n")
        assert b"main must not execute" not in text.stdout and b"base = 17" in text.stdout
        selected = records(run(binary, ["test", "--json", "--only", "Main.test.5",
                                        "--only", "Main.test.1", "--only", "Main.test.1",
                                        str(path)], 0), "test")
        assert [r["name"] for r in selected[1:-1]] == ["Main.test.1", "Main.test.5"]
        assert selected[-1]["passed"] == 2
        # Copying count is legal; capturing count itself across its later write
        # is not a snapshot guarantee the checker can certify.
        path.write_text(source.replace("copy = count\n", "").replace("fn() copy * 3 end", "fn() count * 3 end"))
        rejected = records(run(binary, ["test", "--json", str(path)], 1), "test")
        assert rejected[1]["code"] == "E0092"
        assert all(rejected[-1][key] == 0 for key in ("passed", "failed", "stopped"))
        path.write_text(source.replace('stop "main must not execute"', 'make(3, 29)(5)()'))
        result = run(binary, ["run", str(path)], 60)
        assert not result.stdout and not result.stderr
        for codec in ("core", "bytecode"):
            emitted = run(binary, ["check", f"--emit={codec}", str(path)], 0)
            assert emitted.stdout and not emitted.stderr
            artifact = pathlib.Path(directory) / f"capture.{codec}"
            artifact.write_bytes(emitted.stdout)
            decoded = run(binary.parent / "tests/test_cli.test", [f"--roundtrip-{codec}", str(artifact)], 0)
            assert not decoded.stdout and not decoded.stderr
    print(f"{binary}: escaped captures, snapshots, stopped spans and codec roundtrips passed")


def imports(binary):
    # Aliasing a listed import and qualifying the same declaration must agree;
    # asymmetric arguments distinguish canonical binding from swapped inputs.
    source = ('module Main\nuse Shapes{make, score, ratio}\n'
              'test "imported aliases"\nf = score\n'
              'assert f(make(3), 11) + Shapes.score(Shapes.make(5), 2) == 69\nend\n'
              'test "wrong imported answer"\nassert score(make(3), 11) == 31\nend\n'
              'test "imported escaped stop"\naction = ratio(17)\nassert action(0) == 1\nend\n'
              'test "after stop"\nassert score(make(2), 3) == 17\nend\n'
              'fn main() : Int\n'
              'f = score\nf(make(3), 11) + Shapes.score(Shapes.make(5), 2)\nend\n')
    shapes = ('module Shapes\nexpose Box(..), make, score, ratio\nstruct Box\nvalue: Int\nend\n'
              'fn make(value: Int) : Box\nBox(value: value)\nend\n'
              'fn score(box: Box, extra: Int) : Int\nexamples\nscore(make(3), 11) == 32\nend\n'
              'box.value * 7 + extra\nend\n'
              'fn ratio(base: Int) : fn(Int) : Int\nfn(value) base / value end\nend\n'
              'test "module snapshot"\nvar count = 13\ncopy = count\n'
              'show = fn() copy * 3 end\ncount += 7\nassert show() + count == 59\nend\n')
    with tempfile.TemporaryDirectory(prefix="mo-cli-imports-") as directory:
        path = pathlib.Path(directory) / "main.mo"
        owner = path.parent / "shapes.mo"
        path.write_text(source)
        owner.write_text(shapes)
        checked = run(binary, ["check", str(path)], 0)
        assert not checked.stdout and not checked.stderr
        # --in uses the declaration's module, not the importing entry module.
        listed = records(run(binary, ["test", "--json", "--list", "--in", "Shapes", str(path)], 0), "test")
        assert [r["name"] for r in listed[1:-1]] == ["Shapes.score.example.1", "Shapes.test.1"]
        start = shapes.index("score(make(3), 11) == 32")
        assert listed[1]["span"] == {"file": str(owner), "start": start, "end": start + 24,
                                      "line": 11, "column": 1, "end_line": 11, "end_column": 25}
        start = shapes.index('test "module snapshot"')
        assert listed[2]["span"] == {"file": str(owner), "start": start, "end": len(shapes) - 1,
                                      "line": 18, "column": 1, "end_line": 24, "end_column": 4}
        assert all(listed[-1][key] == 0 for key in ("errors", "passed", "failed", "stopped"))
        selected = records(run(binary, ["test", "--json", "--in", "Shapes",
                                        "--only", "Shapes.test.1", "--only", "Shapes.score.example.1",
                                        "--only", "Shapes.test.1", str(path)], 0), "test")
        assert [r["name"] for r in selected[1:-1]] == ["Shapes.score.example.1", "Shapes.test.1"]
        assert all(r["outcome"] == "passed" for r in selected[1:-1])
        assert selected[-1]["passed"] == 2 and selected[-1]["errors"] == 0
        for selectors in (["--in", "Shapes", "--only", "Main.test.1"], ["--in", "Shape"]):
            mistake = run(binary, ["test", "--json", *selectors, str(path)], 2)
            assert not mistake.stdout and mistake.stderr.count(b"\n") == 1
        items = records(run(binary, ["test", "--json", str(path)], 1), "test")
        by_name = {r["name"]: r for r in items[1:-1]}
        assert {name: r["outcome"] for name, r in by_name.items()} == {
            "Main.test.1": "passed", "Main.test.2": "failed", "Main.test.3": "stopped",
            "Main.test.4": "passed", "Shapes.score.example.1": "passed", "Shapes.test.1": "passed"}
        failed = by_name["Main.test.2"]
        assert (failed["left"], failed["right"]) == ("32", "31")
        start = source.index("assert score(make(3), 11) == 31")
        assert failed["line"] == {"file": str(path), "start": start, "end": start + 31,
                                   "line": 8, "column": 1, "end_line": 8, "end_column": 32}
        defect = by_name["Main.test.3"]["defect"]
        start = shapes.index("base / value")
        assert defect["reason"] == "dividing by zero"
        assert defect["span"] == {"file": str(owner), "start": start, "end": start + 12,
                                   "line": 16, "column": 11, "end_line": 16, "end_column": 23}
        assert [f["function"] for f in defect["stack"]] == ["Shapes.ratio.fn.1", "Main.test.3"]
        assert [f["span"]["file"] for f in defect["stack"]] == [str(owner), str(path)]
        assert defect["variables"] == [{"name": "base", "value": "17"}, {"name": "value", "value": "0"}]
        assert items[-1] == {"record": "totals", "errors": 0, "dropped": 0,
                             "passed": 4, "failed": 1, "stopped": 1, "status": 1}
        text = run(binary, ["test", str(path)], 1)
        assert not text.stderr and text.stdout.endswith(b"4 passed, 1 failed, 1 stopped\n")
        assert b"base = 17" in text.stdout and b"shapes.mo:16:11" in text.stdout
        result = run(binary, ["run", str(path)], 69)
        assert not result.stdout and not result.stderr
        for codec in ("core", "bytecode"):
            emitted = run(binary, ["check", f"--emit={codec}", str(path)], 0)
            assert emitted.stdout and not emitted.stderr
            artifact = path.parent / f"imports.{codec}"
            artifact.write_bytes(emitted.stdout)
            decoded = run(binary.parent / "tests/test_cli.test", [f"--roundtrip-{codec}", str(artifact)], 0)
            assert not decoded.stdout and not decoded.stderr
        # ratio is used only by a test. Removing that test must diagnose the
        # original listed import before resolving even a deliberately bad name.
        path.write_text(source.replace('test "imported escaped stop"\naction = ratio(17)\n'
                                       'assert action(0) == 1\nend\n', ''))
        rejected = records(run(binary, ["test", "--json", "--list", "--only", "Typo.test.1", str(path)], 1), "test")
        assert rejected[1]["code"] == "E0093" and rejected[1]["section"] == "6.1"
        assert {key: rejected[1]["primary"][key] for key in
                ("file", "start", "end", "line", "column", "end_line", "end_column")} == {
                    "file": str(path), "start": 36, "end": 41, "line": 2, "column": 25,
                    "end_line": 2, "end_column": 30}
        assert rejected[-1] == {"record": "totals", "errors": 1, "dropped": 0,
                                "passed": 0, "failed": 0, "stopped": 0, "status": 1}
        for command in (["check"], ["run"], ["check", "--emit=core"], ["check", "--emit=bytecode"]):
            rejected_text = run(binary, [*command, str(path)], 1)
            assert not rejected_text.stdout and b"E0093" in rejected_text.stderr
    print(f"{binary}: imported aliases, observation owners, stopped spans and unused-import preflight passed")


def generic_imports(binary):
    # The caller's A/Z keys deliberately reverse the callee's Z/A keys.
    # The alias must carry the callee signature and the caller's actual proofs.
    source = ('module Main\nuse Calc\nuse Bridge{forward, concrete}\n'
              'struct Label\nvalue: Int\nend\n'
              'fn to_text(label: Label) : Text\n"#{label.value * 3}"\nend\n'
              'fn relay(value: A, tag: Z) : A where A: Number, Z: Text\n'
              'f = Calc.choose\nf(value, tag)\nend\n'
              'fn wrap(f: fn(Int) : fn(Int) : Int, base: Int) : fn(Int) : Int\n'
              'fn(extra) f(base)(extra) end\nend\n'
              'test "swapped keys"\nassert relay(13, true) * 3 + relay(17, false) == 95\nend\n'
              'test "equal and mixed actuals"\nf = Calc.pair\n(a, b) = f(3, 17)\n'
              '(c, d) = f(1.25, 19)\nassert a * 10 + b == 111\nassert c == 2.5 and d == 57\nend\n'
              'test "caller customizer"\nassert Calc.show(Label(value: 7)) == "21"\nend\n'
              'test "escaped generic frames"\nfactory = Calc.offset\na = forward(3)\n'
              'b = concrete(5)\nc = wrap(factory, 17)\n'
              'assert a(11) + b(7) + c(29) == 72\nassert Calc.offset(1.25)(2.5) == 3.75\nend\n'
              'test "wrong generic answer"\naction = Calc.offset(3)\nassert action(11) == 15\nend\n'
              'test "escaped generic stop"\naction = Calc.ratio(23)\nassert action(0) == 1\nend\n'
              'test "after stop"\nf = Calc.twice\nassert f(17) == 34 and f(1.25) == 2.5\nend\n'
              'fn main() : Int\nrelay(13, true) * 3 + relay(17, false)\nend\n')
    calc = ('module Calc\nexpose choose, pair, show, offset, ratio, twice\n'
            'fn choose(value: Z, tag: A) : Z where Z: Number, A: Text\n'
            'if "#{tag}" == "true": value + value else: value\nend\n'
            'fn pair(left: Z, right: A) : (Z, A) where Z: Number, A: Number\n'
            '(left + left, right + right + right)\nend\n'
            'fn show(value: T) : Text where T: Text\n"#{value}"\nend\n'
            'fn offset(value: T) : fn(T) : T where T: Number\nfn(extra) value + extra end\nend\n'
            'fn ratio(base: T) : fn(T) : T where T: Number\nfn(value) base / value end\nend\n'
            'fn twice(value: T) : T where T: Number\nexamples\ntwice(3) == 6\ntwice(1.25) == 2.5\n'
            'end\nvalue + value\nend\n')
    bridge = ('module Bridge\nexpose forward, concrete\nuse Calc{offset}\n'
              'fn forward(value: U) : fn(U) : U where U: Number\nf = offset\nf(value)\nend\n'
              'fn concrete(value: Int) : fn(Int) : Int\noffset(value)\nend\n')

    def expected_span(path, text, row):
        # These fixture strings are ASCII: byte offsets and character columns
        # are independently derived from their exact source, not compiler data.
        start = text.index(row)
        line = text.count('\n', 0, start) + 1
        column = start - text.rfind('\n', 0, start)
        return {"file": str(path), "start": start, "end": start + len(row),
                "line": line, "column": column, "end_line": line, "end_column": column + len(row)}

    with tempfile.TemporaryDirectory(prefix="mo-cli-generic-imports-") as directory:
        path = pathlib.Path(directory) / "main.mo"
        owner = path.parent / "calc.mo"
        path.write_text(source)
        owner.write_text(calc)
        (path.parent / "bridge.mo").write_text(bridge)
        checked = run(binary, ["check", str(path)], 0)
        assert not checked.stdout and not checked.stderr
        listed = records(run(binary, ["test", "--json", "--list", "--in", "Calc", str(path)], 0), "test")
        assert [r["name"] for r in listed[1:-1]] == ["Calc.twice.example.1", "Calc.twice.example.2"]
        for item, row in zip(listed[1:-1], ("twice(3) == 6", "twice(1.25) == 2.5")):
            assert item["span"] == expected_span(owner, calc, row)
        selected = records(run(binary, ["test", "--json", "--in", "Calc",
                                        "--only", "Calc.twice.example.2", "--only", "Calc.twice.example.1",
                                        "--only", "Calc.twice.example.2", str(path)], 0), "test")
        assert [r["name"] for r in selected[1:-1]] == ["Calc.twice.example.1", "Calc.twice.example.2"]
        assert all(r["outcome"] == "passed" for r in selected[1:-1])
        assert selected[-1]["passed"] == 2 and selected[-1]["errors"] == 0
        items = records(run(binary, ["test", "--json", str(path)], 1), "test")
        by_name = {r["name"]: r for r in items[1:-1]}
        assert {name: r["outcome"] for name, r in by_name.items()} == {
            **{f"Main.test.{n}": "passed" for n in (1, 2, 3, 4, 7)},
            "Main.test.5": "failed", "Main.test.6": "stopped",
            "Calc.twice.example.1": "passed", "Calc.twice.example.2": "passed"}
        failed = by_name["Main.test.5"]
        assert (failed["left"], failed["right"]) == ("14", "15")
        assert failed["line"] == expected_span(path, source, "assert action(11) == 15")
        defect = by_name["Main.test.6"]["defect"]
        assert defect["reason"] == "dividing by zero"
        assert defect["span"] == expected_span(owner, calc, "base / value")
        assert [f["function"] for f in defect["stack"]] == ["Main::Calc.ratio(Int).fn.1", "Main.test.6"]
        assert [f["span"]["file"] for f in defect["stack"]] == [str(owner), str(path)]
        assert defect["variables"] == [{"name": "base", "value": "23"}, {"name": "value", "value": "0"}]
        assert items[-1] == {"record": "totals", "errors": 0, "dropped": 0,
                             "passed": 7, "failed": 1, "stopped": 1, "status": 1}
        text = run(binary, ["test", str(path)], 1)
        assert not text.stderr and text.stdout.endswith(b"7 passed, 1 failed, 1 stopped\n")
        assert b"base = 23" in text.stdout and b"Main::Calc.ratio(Int).fn.1" in text.stdout
        result = run(binary, ["run", str(path)], 95)
        assert not result.stdout and not result.stderr
        for codec in ("core", "bytecode"):
            emitted = run(binary, ["check", f"--emit={codec}", str(path)], 0)
            assert emitted.stdout and not emitted.stderr
            artifact = path.parent / f"generic-imports.{codec}"
            artifact.write_bytes(emitted.stdout)
            decoded = run(binary.parent / "tests/test_cli.test", [f"--roundtrip-{codec}", str(artifact)], 0)
            assert not decoded.stdout and not decoded.stderr
        # A missing actual ability is a source error, not a selection mistake;
        # it must suppress all observations and artifacts before any execution.
        path.write_text(source.replace('struct Label\n', 'struct Token\nvalue: Int\nend\nstruct Label\n')
                        .replace('test "swapped keys"\n', 'test "missing actual ability"\n'
                                 'f = Calc.choose\nassert f(Token(value: 3), true) == Token(value: 6)\nend\n'
                                 'test "swapped keys"\n'))
        rejected = records(run(binary, ["test", "--json", "--list", "--only", "Typo.test.1", str(path)], 1), "test")
        assert rejected[1]["code"] == "E0080" and rejected[-1]["errors"] == 1
        assert all(rejected[-1][key] == 0 for key in ("passed", "failed", "stopped"))
        for command in (["check"], ["run"], ["check", "--emit=core"], ["check", "--emit=bytecode"]):
            refused = run(binary, [*command, str(path)], 1)
            assert not refused.stdout and b"E0080" in refused.stderr
    print(f"{binary}: imported generic keys, caller proofs, source observations, stops and ability preflight passed")


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

        # Loading/checking failure is never run as code. Missing main is an
        # error even for check; an inference result alone is not a certificate.
        for body, code in (("fn f() : Int\n3\nend", "E0079"),
                           ('fn main() : Int\n"text"\nend', "E0078")):
            path.write_text("module Main\n" + body + "\n")
            for command in ("check", "run"):
                result = run(binary, [command, str(path)], 1)
                assert not result.stdout and code.encode() in result.stderr
            items = records(run(binary, ["check", "--json", str(path)], 1), "check")
            assert items[1]["code"] == code and items[-1]["dropped"] == 0

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
        # An earlier-global alias and nearest-lambda try used to emit invalid
        # Core; a permitted closure must not be rejected with deferred globals.
        for body, status in (
            ('base = 13\nhandler = first\nanswer = handler()\n'
             'reader = fn() Some(try Some(1)) end\nfn first() : Int\nbase\nend\n'
             'fn main() : Int\nanswer + (reader() or 9)\nend\n', 14),
            ('base = 13\nhandler = fn() base end\n'
             'fn main() : Int\nhandler() + 24\nend\n', 37),
            ('answer = handler(-7)\nhandler = Int.abs\n'
             'fn main() : Int\nanswer + 30\nend\n', 37),
            ('fn main() : Int\nInt.min(7, -11) * 2 + Int.max(-11, 7) + 68\nend\n', 53),
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
        # Int.pow is separate from the supported abs/min/max native routes.
        for body, reason in (
            ('handler = fn() base end\nbase = 13\n'
             'fn main() : Int\nhandler() + 24\nend\n', "deferred function"),
            ('answer = handler()\nhandler = first\nbase = 13\n'
             'fn first() : Int\nbase\nend\nfn main() : Int\nanswer + 24\nend\n',
             "deferred function"),
            ('handler = first\nfn first() : Int\nhandler()\nend\n'
             'fn main() : Int\n17\nend\n', "deferred function"),
            ('answer = handler(2, 3)\nhandler = Int.pow\n'
             'fn main() : Int\nanswer + 30\nend\n', "native call"),
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
            os.mkdir(folder)
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
            assert diag["primary"]["start"] == source.index(b"allows")
            assert diag["secondary"][0]["line"] == 4
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


flags = {
    "--json": ([], {"check", "test"}),
    "--list": ([], {"test"}),
    "--reference": ([], {"guide"}),
    "--only": (["Main.test.1"], {"test"}),
    "--in": (["Main"], {"test"}),
    "--max-stack": (["1MiB"], {"run", "test"}),
    "--max-depth": (["100"], {"run", "test"}),
    "--max-heap": (["1GiB"], {"run", "test"}),
    "--calls": (["30"], {"test"}),
    "--seconds": (["3"], {"test"}),
    "--emit=core": ([], {"check"}),
    "--emit=bytecode": ([], {"check"}),
}


def check(binary):
    driver = binary.parent / "tests/test_cli.test"
    faults = binary.parent / "tests/test_cli_faults.test"
    runner(binary)
    anchors(binary)
    constants(binary)
    filenames(binary)
    observations(binary)
    captures(binary)
    imports(binary)
    generic_imports(binary)
    scalar_promises(binary)
    items = records(run(faults, ["--json"], 1), "check")
    assert items[1]["code"] == "E1401" and items[-1]["dropped"] == 0
    assert items[1]["primary"]["file"] is None and items[1]["section"] is None
    failure = run(faults, ["--text"], 1)
    assert not failure.stdout and b"E1401" in failure.stderr
    for stage in ("parse", "load", "modules", "signatures", "bodies", "fix", "multiline"):
        path = f"tests/cli/{stage}/main.mo"
        for json_mode in (False, True):
            result = run(binary, ["check", *(["--json"] if json_mode else []), path], 1)
            actual = result.stdout if json_mode else result.stderr
            expected = root / f"tests/cli/{stage}/{'json' if json_mode else 'text'}.golden"
            assert actual == expected.read_bytes(), (binary, expected, actual)
            if json_mode:
                items = records(result, "check")
                if stage == "bodies":
                    assert [d["primary"]["line"] for d in items[1:-1]] == [3, 6]
                if stage == "fix":
                    edit = items[1]["fixes"][0]["edits"][0]
                    # 12 bytes for the header + 7 for "fn run(".
                    assert edit["text"] == "_" and edit["span"]["start"] == edit["span"]["end"] == 19
            else:
                assert not result.stdout
    good = "tests/cli/good/main.mo"
    result = run(binary, ["check", good], 0)
    assert not result.stdout and not result.stderr
    records(run(binary, ["check", "--json", good], 0), "check")
    for mode in ("text", "json"):
        result = run(driver, [f"--render-{mode}"], 0)
        assert not result.stderr
        assert result.stdout == (root / f"tests/cli/render-{mode}.golden").read_bytes()
        if mode == "json":
            d = json.loads(result.stdout)
            assert d["primary"]["column"] == 2
            assert d["primary"]["file"] == 'badÿ"\\.mo'
            assert d["secondary"][0]["line"] == 7
            assert d["secondary"][1]["file"] is None
    escaped = json.loads(run(driver, ["--render-bytes"], 0).stdout)
    assert escaped["headline"] == "".join(chr(i) for i in range(1, 256))
    assert escaped["primary"]["label"] == "é界😀"
    assert run(binary, ["guide", "--reference"], 0).stdout == (root / "docs/reference/README.md").read_bytes()
    guide = root / "docs/agent-guide.md"
    guide_bytes = guide.read_bytes() if guide.exists() else b""
    if guide_bytes:
        assert run(binary, ["guide"], 0).stdout == guide_bytes
    else:
        result = run(binary, ["guide"], 1)
        assert not result.stdout and b"--reference" in result.stderr
    for command in ("fix",):
        result = run(binary, [command, good], 1)
        assert not result.stdout and b"this build cannot" in result.stderr
        assert result.stderr.count(b"<unavailable>") == 1
    result = run(binary, ["test", "--json", "--list", "--only", "No.match", good], 2)
    assert not result.stdout and result.stderr.count(b"\n") == 1

    def mistake(args, message=None):
        r = run(binary, args, 2)
        assert not r.stdout and r.stderr.count(b"\n") == 1, (args, r)
        if message:
            assert r.stderr == message.encode() + b"\n", (args, r.stderr)

    for args in ([], ["unknown"], [""], ["--version", "extra"], ["check"], ["format"],
                 ["fix"], ["run"], ["test"], ["guide", good], ["check", ""],
                 ["check", good, good], ["test", good, good], ["check", "--bad", good],
                 ["check", "--emit=bad", good], ["check", "--emit", "core", good],
                 ["check", "--json", "--emit=core", good], ["check", "--version", good],
                 ["check", "--json", "--bad", good]):
        mistake(args)
    mistake(["bad\n\t\xff"], 'unknown command (expected check, run, test, format, fix or guide): "bad\\x0a\\x09\\xc3\\xbf"')
    for flag, (value, allowed) in flags.items():
        for command in ("check", "run", "test", "format", "fix", "guide"):
            args = [command, flag, *value, *([] if command == "guide" else [good])]
            if command not in allowed:
                mistake(args, f'flag is not accepted by this command: "{flag}"')
            else:
                run(binary, args, 2 if command == "test" else 0)
                if flag != "--only":
                    mistake([command, flag, *value, flag, *value, *([] if command == "guide" else [good])])
        if value:
            for missing in ([], [""], ["--json"]):
                suffix = ''.join(f' "{v}"' for v in missing)
                mistake(["test", flag, *missing], f'flag needs a nonempty value: "{flag}"{suffix}')
    for flag in ("--max-stack", "--max-depth", "--max-heap", "--calls", "--seconds"):
        for value in ("0", "-1", "+1", " 1", "1 ", "1.5", "1KB", "1mib", "18446744073709551616"):
            mistake(["test", flag, value, good])
    mistake(["test", "--max-heap", "-1", good],
            'invalid limit (expected a positive decimal integer; bytes may use B, KiB, MiB or GiB): "--max-heap" "-1"')
    for flag in ("--max-depth", "--calls", "--seconds"):
        mistake(["test", flag, "4294967296", good])
        mistake(["test", flag, "1B", good])
    for flag in ("--max-stack", "--max-heap"):
        mistake(["test", flag, "18014398509481984KiB", good])
        for value in ("1", "1B", "1KiB", "1MiB", "1GiB", "18446744073709551615"):
            run(binary, ["test", flag, value, good], 2)
    run(binary, ["run", good, "--bad", ""], 0)
    run(binary, ["test", "--only", "a", "--only", "b", "--in", "Main", good], 2)
    run(binary, ["format", good, good], 0)
    run(binary, ["fix", good, good], 1)
    for version in ("--version", "-V"):
        assert run(binary, [version], 0).stdout == b"mo 0.0.0\n"
    read_fd, write_fd = os.pipe()
    os.close(read_fd)
    try:
        result = subprocess.run([str(binary), "guide", "--reference"], stdout=write_fd,
                                stderr=subprocess.PIPE, timeout=30)
        assert result.returncode == 0 and not result.stderr
    finally:
        os.close(write_fd)
    run(binary, ["run", good, "x" * 5000], 0)
    run(binary, ["run", good, *(str(i) for i in range(300))], 0)
    run(binary, ["run", good, *("x" * 4096 for _ in range(16))], 0)
    for command in ("format", "fix"):
        run(binary, [command, *([good] * 4096)], 0 if command == "format" else 1)
    run(binary, ["test", *(["--only", "x"] * 128), good], 2)
    mistake(["test", *(["--only", "x"] * 129), good])
    mistake(["test", "--only", "x" * 4097, good])
    run(binary, ["test", *(["--only", "x" * 4096] * 15), good], 2)
    mistake(["test", *(["--only", "x" * 4096] * 16), good])
    # Unavailable locations, invalid source bytes, invalid POSIX path bytes,
    # and the -- delimiter are exercised against real files, not mocks.
    records(run(binary, ["check", "--json", "tests/cli/missing.mo"], 1), "check")
    with tempfile.TemporaryDirectory() as temp:
        folder = pathlib.Path(temp)
        source_stack(binary, folder)
        encoded = folder / "caf\u00e9-ascii"
        encoded.mkdir()
        path = encoded / "main.mo"
        path.write_text('module Main\nfn run() : Int\n"Jos\u00e9 \u65e5\u672c\u8a9e \U0001f600\u00db"\nend\n',
                        encoding="utf-8")
        for terminal in ("C", "POSIX", "mo_locale_that_does_not_exist"):
            result = subprocess.run([str(binary), "check", str(path)], cwd=root,
                                    env={**os.environ, "LC_ALL": terminal},
                                    capture_output=True, timeout=30)
            assert result.returncode == 1 and not result.stdout
            result.stderr.decode("ascii")
            assert b"caf\\u{0e9}" in result.stderr and b"Jos\\u{0e9}" in result.stderr
        unsafe = folder / "bad\u009b\u202e\U000e0001"
        unsafe.mkdir()
        path = unsafe / "main.mo"
        path.write_text("module Main\nfn run()\nunknown\nend\n")
        result = run(binary, ["check", "--json", str(path)], 1)
        assert b"\\u009b\\u202e\\udb40\\udc01" in result.stdout
        assert records(result, "check")[1]["primary"]["file"] == str(path)
        unicode_folder = folder / "café"
        unicode_folder.mkdir()
        unicode_path = unicode_folder / "main.mo"
        unicode_path.write_text('module Main\nfn run() : Text\n  -"José 日本語 😀"\nend\n',
                                encoding="utf-8")
        result = run(binary, ["check", str(unicode_path)], 1,
                     env={**os.environ, "LC_ALL": utf8_locale})
        assert not result.stdout
        assert str(unicode_path).encode("utf-8") in result.stderr
        assert '-"José 日本語 😀"'.encode("utf-8") in result.stderr
        d = records(run(binary, ["check", "--json", str(unicode_path)], 1), "check")[1]
        assert d["primary"]["file"] == str(unicode_path)
        # The string occupies twelve original characters, not its byte length
        # or displayed width. Unary minus refuses this Text operand.
        assert d["primary"]["column"] == 4 and d["primary"]["end_column"] == 16
        path = folder / "main.mo"
        path.write_bytes(b"module Main\n\xff\n")
        d = records(run(binary, ["check", "--json", str(path)], 1), "check")[1]
        assert d["code"] == "E0001" and d["primary"]["line"] == 2
        invalid = folder / "bad\udcff"
        try:
            invalid.mkdir()
        except OSError as error:
            # APFS/HFS+ refuse non-UTF-8 names; don't hide unrelated I/O errors.
            if error.errno != errno.EILSEQ:
                raise
            print(f"{binary}: filesystem refuses non-UTF-8 names; path case skipped")
        else:
            path = invalid / "main.mo"
            path.write_text("module Main\nfn run()\nunknown\nend\n")
            d = records(run(binary, ["check", "--json", str(path)], 1), "check")[1]
            assert "badÿ" in d["primary"]["file"]
        path = folder / "main.mo"
        path.write_text("module Main\n" + "".join(f"fn f{i}()\nunknown\nend\n" for i in range(1500)))
        items = records(run(binary, ["check", "--json", str(path)], 1), "check")
        assert items[-1]["errors"] == 1000 and items[-1]["dropped"] == 500
        run(binary, ["format", "--", "-file.mo"], 1)
        # The printer must refuse rather than introduce a lexer limit error.
        for letter in (b'a', '界'.encode()):
            for length in (65532, 65533):
                source = (b'module Main\nfn f() : Text\n"' + letter * length
                          + b'"\nend\nfn main()\nend\n')
                path.write_bytes(source)
                result = run(binary, ["format", str(path)], 0 if length == 65532 else 1)
                assert not result.stdout
                if length == 65533:
                    assert b"E1403" in result.stderr and path.read_bytes() == source
                else:
                    assert not result.stderr and b'\n  "' in path.read_bytes()
                    run(binary, ["check", str(path)], 0)
        # Alignment adds 49 spaces to the short code row. Each input row is
        # legal; only the aligned result crosses the scalar limit in case two.
        for length in (65479, 65480):
            source = (b'module Main\na=0 #' + b'c' * length + b'\n' +
                      b'b' * 50 + b'=0 # short\n')
            path.write_bytes(source)
            result = run(binary, ["format", str(path)], 0 if length == 65479 else 1)
            assert not result.stdout
            if length == 65480:
                assert b'E1403' in result.stderr and path.read_bytes() == source
            else:
                expected = (b'module Main\na = 0' + b' ' * 51 + b'#' + b'c' * length +
                            b'\n' + b'b' * 50 + b' = 0  # short\n')
                assert not result.stderr and path.read_bytes() == expected
                # A formatter fragment still has no entry. Supporting its
                # constants must not silently waive whole-check's main rule.
                result = run(binary, ["check", str(path)], 1)
                assert b'E0079' in result.stderr
        # Rename is observable: an existing reader and hard link keep the old
        # complete file, while the requested path gets the complete new file.
        source = b'module Main\n\n\nfn f()\nvar x=1\nx=x+2\nx\nend\n'
        expected = b'module Main\n\nfn f()\n  var x = 1\n  x += 2\n  x\nend\n'
        path.write_bytes(source)
        path.chmod(0o640)
        alias = folder / 'alias.mo'
        os.link(path, alias)
        with path.open('rb') as old_reader:
            result = run(binary, ["format", str(path)], 0)
            assert not result.stdout and not result.stderr
            assert path.read_bytes() == expected and old_reader.read() == source
        assert alias.read_bytes() == source
        assert stat.S_IMODE(path.stat().st_mode) == 0o640
        before = path.stat()
        run(binary, ["format", str(path)], 0)
        after = path.stat()
        assert before.st_ino == after.st_ino and before.st_mtime_ns == after.st_mtime_ns
        bad = folder / 'bad.mo'
        bad_source = b'module Bad\nfn f()\n(\nend\n'
        bad.write_bytes(bad_source)
        path.write_bytes(source)
        result = run(binary, ["format", str(bad), str(path)], 1)
        assert not result.stdout and b'E' in result.stderr
        assert bad.read_bytes() == bad_source and path.read_bytes() == expected
        symlink = folder / 'linked.mo'
        symlink.symlink_to(alias)
        fifo = folder / 'pipe.mo'
        os.mkfifo(fifo)
        result = run(binary, ["format", str(symlink), str(fifo), str(folder)], 1)
        assert result.stderr.count(b'E1402') == 3 and not result.stdout
        assert symlink.is_symlink() and alias.read_bytes() == source
        assert not list(folder.glob('.mo-format-*'))
    print(f"{binary}: CLI goldens, argument matrix and JSON validity passed")


for executable in sys.argv[1:]:
    check(pathlib.Path(executable).resolve())
