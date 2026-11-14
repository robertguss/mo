import os
import pathlib
import tempfile

from cli_common import records, run


def imported_constants(binary):
    with tempfile.TemporaryDirectory(prefix='mo-imported-constants-') as directory:
        path = pathlib.Path(directory) / 'main.mo'
        origin = path.parent / 'origin.mo'
        origin.write_text('module Origin\nexpose base, greeting, handler\nbase = 13\n'
                          'greeting = "λ\\u{0}owned"\nhandler = make(13)\n'
                          'fn make(offset: Int) : fn(Int) : Int\n'
                          'fn(extra) base + offset + extra end\nend\n'
                          'test "declaring constant observation"\nassert handler(3) == 29\nend\n')
        path.write_text('module Main\nuse Origin{base, greeting, handler}\n'
                        'fn probe(_values: List(Text)) : Int\npromise "foreign constant closure"\n'
                        'result == 52\nallows _values: ["owned"], result: 52\n'
                        'forbids _values: ["é"], result: 53\nend\n'
                        'Origin.base + base + handler(0)\nend\n'
                        'test "foreign owned text"\nassert greeting == "λ\\u{0}owned"\nend\n'
                        'fn main() : Int\nprobe(["main"])\nend\n')
        items = records(run(binary, ['test', '--json', str(path)], 0), 'test')
        assert [r['name'] for r in items[1:-1]] == [
            'Main.probe.promise.1', 'Main.test.1', 'Origin.test.1']
        assert items[-1]['passed'] == 3 and items[-1]['errors'] == 0
        assert items[3]['span']['file'] == str(origin)
        result = run(binary, ['run', str(path)], 52)
        assert not result.stdout and not result.stderr

        # A direct imported call has certified initializer dependencies. Calling
        # the returned closure inside an initializer remains a separate refusal.
        origin.write_text('module Origin\nexpose base, greeting, evaluate, freeze\nbase = 13\n'
                          'greeting = "λ\\u{0}owned"\nfn evaluate(n: Int) : Int\nn + base * 2\nend\n'
                          'fn freeze(value: Text) : fn() : Text\nfn() value end\nend\n')
        bridge = path.parent / 'bridge.mo'
        bridge.write_text('module Bridge\nexpose answer, snapshot, probe\n'
                          'use Origin{base, greeting, evaluate, freeze}\n'
                          'answer = evaluate(3) + base\nsnapshot = freeze(greeting)\n'
                          'fn probe(_values: Option(List(Text))) : Int\n'
                          'promise "dependency initializer"\nresult == 42\n'
                          'allows _values: Some(["owned"]), result: 42\n'
                          'forbids _values: None, result: 43\nend\nanswer\nend\n')
        path.write_text('module Main\nuse Bridge{answer, snapshot, probe}\n'
                        'test "dependency constants"\nassert answer == 42\n'
                        'assert snapshot() == "λ\\u{0}owned"\nend\n'
                        'fn main() : Int\nprobe(None)\nend\n')
        items = records(run(binary, ['test', '--json', str(path)], 0), 'test')
        assert [r['name'] for r in items[1:-1]] == ['Main.test.1', 'Bridge.probe.promise.1']
        assert items[-1]['passed'] == 2 and items[-1]['errors'] == 0
        assert items[2]['span']['file'] == str(bridge)
        result = run(binary, ['run', str(path)], 42)
        assert not result.stdout and not result.stderr
        for codec in ('core', 'bytecode'):
            emitted = run(binary, ['check', '--emit=' + codec, str(path)], 0)
            artifact = path.parent / ('initialized.' + codec)
            artifact.write_bytes(emitted.stdout)
            run(binary.parent / 'tests/test_cli.test', ['--roundtrip-' + codec, str(artifact)], 0)

        # The subject returns an imported owned nominal constant. The smallest
        # failure needs true, not the minimum false; its original second line
        # and Unicode/NUL result survive copying and shrinking in fresh heaps.
        source = ('module Origin\nexpose Pair(..), packet, probe\nstruct Pair(Z, A)\n'
                  'zeta: Z\nalpha: A\nend\npacket = Pair(alpha: false, zeta: "λ\\u{0}owned")\n'
                  'snapshot = fn() packet.zeta end\n'
                  'fn probe(_value: Pair(Text, Bool), _values: List(Text)) : Pair(Text, Bool)\n'
                  'promise "retained imported constant"\nresult.zeta == snapshot()\n'
                  'result.alpha == _value.alpha\n'
                  'allows _values: [], _value: Pair(zeta: "anchor", alpha: false), result: packet\n'
                  'forbids _value: Pair(alpha: true, zeta: "é"), _values: ["owned"], result: packet\n'
                  'end\npacket\nend\n')
        origin.write_text(source)
        path.write_text('module Main\nuse Origin{Pair, packet, probe}\n'
                        'test "initialized owned constant"\n'
                        'assert packet.zeta == "λ\\u{0}owned" and not packet.alpha\nend\n'
                        'fn main() : Int\nif not probe(Pair(zeta: "main", alpha: false), []).alpha: 37 else: 1\nend\n')
        items = records(run(binary, ['test', '--json', str(path)], 1), 'test')
        failure = items[2]
        assert items[1]['name'] == 'Main.test.1' and items[1]['outcome'] == 'passed'
        assert failure['name'] == 'Origin.probe.promise.1' and failure['outcome'] == 'failed'
        assert failure['input'] == [{'name': '_value', 'value': 'Pair(zeta: "", alpha: true)'},
                                    {'name': '_values', 'value': '[]'}]
        assert failure['result'] == r'Pair(zeta: "λ\u{0}owned", alpha: false)'
        predicate = b'result.alpha == _value.alpha'
        start = source.encode().index(predicate)
        assert failure['line']['file'] == str(origin)
        assert (failure['line']['start'], failure['line']['end']) == (start, start + len(predicate))
        assert items[-1]['passed'] == items[-1]['failed'] == 1 and items[-1]['errors'] == 0
        result = run(binary, ['run', str(path)], 37)
        assert not result.stdout and not result.stderr
        for codec in ('core', 'bytecode'):
            emitted = run(binary, ['check', '--emit=' + codec, str(path)], 0)
            artifact = path.parent / ('owned-constant.' + codec)
            artifact.write_bytes(emitted.stdout)
            run(binary.parent / 'tests/test_cli.test', ['--roundtrip-' + codec, str(artifact)], 0)

        # Initialization is before anchors/observations and outside the promise
        # clock. Unsupported selected inputs must still refuse before it.
        origin.write_text('module Origin\nexpose seed\nseed = abort()\nlater = second()\n'
                          'fn abort() : Int\nstop "foreign initializer stopped"\nend\n'
                          'fn second() : Int\nstop "later initializer stopped"\nend\n')
        ordinary = ('test "not counted after stopped initializer"\nassert true\nend\n'
                    'fn main() : Int\nseed\nend\n')
        promise = ('fn probe(_values: List(Text)) : Int\n'
                   'promise "initialization precedes promise"\nresult == 13\n'
                   'allows _values: ["owned"], result: 13\nforbids _values: [], result: 14\n'
                   'end\nseed\nend\n')
        for supported in (True, False):
            body = promise if supported else promise.replace('List(Text)', 'List(fn(Int) : Int)').replace(
                '_values: ["owned"]', '_values: []')
            path.write_text('module Main\nuse Origin{seed}\n' + body + ordinary)
            listed = records(run(binary, ['test', '--json', '--list', '--max-heap', '1',
                                           '--seconds', '1', str(path)], 0), 'test')
            assert [r['name'] for r in listed[1:-1]] == ['Main.probe.promise.1', 'Main.test.1']
            items = records(run(binary, ['test', '--json', str(path)], 1), 'test')
            assert len(items) == 3 and items[1]['code'] == ('E1406' if supported else 'E1400')
            assert all(items[-1][key] == 0 for key in ('passed', 'failed', 'stopped'))
            if supported:
                clock = binary.parent / 'tests/test_deadline.test'
                timed = records(run(clock, ['start-failure', 'test', '--json', '--seconds', '1',
                                             str(path)], 1), 'test')
                assert timed[1]['code'] == 'E1406'
                assert items[1]['primary']['file'] == str(origin)
                assert 'foreign initializer stopped' in items[1]['primary']['label']
                assert 'later initializer stopped' not in items[1]['primary']['label']
            else:
                selected = records(run(binary, ['test', '--json', '--only', 'Main.test.1',
                                                str(path)], 1), 'test')
                assert selected[1]['code'] == 'E1406'
        path.write_text('module Main\nuse Origin{seed}\n' + ordinary)
        records(run(binary, ['check', '--json', str(path)], 0), 'check')
        for codec in ('core', 'bytecode'):
            result = run(binary, ['check', '--emit=' + codec, str(path)], 0)
            assert result.stdout and not result.stderr
        result = run(binary, ['run', str(path)], 70)
        assert not result.stdout and b'foreign initializer stopped' in result.stderr
        assert b'later initializer stopped' not in result.stderr and b'Main.main' not in result.stderr
    print(f'{binary}: imported constants, composite retained values and initializer/preflight boundaries passed')


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
            "span": {"file": str(path), "file_bytes": os.fsencode(path).hex(),
                     "start": 74, "end": 105, "line": 6,
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
        assert items[3]["line"] == {"file": str(path), "file_bytes": os.fsencode(path).hex(),
                                    "start": 144, "end": 184,
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

        # Anchors instantiate an otherwise unused generic promise. Listing
        # stays source-based, and ordinary-only selection runs no promise.
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
        accepted = records(run(binary, ["test", "--json", str(path)], 0), "test")
        assert accepted[1]["name"] == "Main.keep.promise.1"
        assert accepted[1]["instance"] == "Main::Main.keep(Int).promise.1"
        assert all(r["outcome"] == "passed" for r in accepted[1:-1])
        assert accepted[-1]["passed"] == 2 and accepted[-1]["errors"] == 0
        accepted = records(run(binary, ["test", "--json", "--only", "Main.test.1", str(path)], 0), "test")
        assert len(accepted) == 3 and accepted[1]["outcome"] == "passed"
        # A buildable generic promise reaches initialization, never main.
        source = path.read_text().replace('test "selected control"\nassert true\nend\n', '')
        path.write_text(source.replace('module Main\n', 'module Main\nseed = crash()\n'
                                       'fn crash() : Int\nstop "must not initialize"\nend\n'
                                       'test "selected control"\nassert true\nend\n'))
        refused = records(run(binary, ["test", "--json", "--only", "Main.test.1",
                                       "--only", "Main.keep.promise.1", str(path)], 1), "test")
        assert len(refused) == 3 and refused[1]["code"] == "E1406"
        assert all(refused[-1][key] == 0 for key in ("passed", "failed", "stopped"))

        source = ('module Main\ntest "Bool, not equality"\nassert (\n  false\n)\nend\n'
                  'test "after Bool failure"\nassert true\nend\nfn main()\nend\n')
        path.write_text(source)
        items = records(run(binary, ["test", "--json", str(path)], 1), "test")
        assert items[1]["outcome"] == "failed" and items[2]["outcome"] == "passed"
        assert "left" not in items[1] and "right" not in items[1]
        start = source.index("assert (")
        assert items[1]["line"] == {"file": str(path), "file_bytes": os.fsencode(path).hex(),
                                    "start": start,
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
        assert defect["span"] == {"file": str(path), "file_bytes": os.fsencode(path).hex(),
                                   "start": start, "end": start + 5,
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
        assert items[3]["line"] == {"file": str(path), "file_bytes": os.fsencode(path).hex(),
                                    "start": start,
                                    "end": start + len("assert action() == 61"),
                                    "line": 23, "column": 1, "end_line": 23, "end_column": 22}
        start = source.index("base / value")
        defect = items[4]["defect"]
        assert defect["reason"] == "dividing by zero"
        assert defect["span"] == {"file": str(path), "file_bytes": os.fsencode(path).hex(),
                                   "start": start,
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
        assert listed[1]["span"] == {"file": str(owner), "file_bytes": os.fsencode(owner).hex(),
                                      "start": start, "end": start + 24,
                                      "line": 11, "column": 1, "end_line": 11, "end_column": 25}
        start = shapes.index('test "module snapshot"')
        assert listed[2]["span"] == {"file": str(owner), "file_bytes": os.fsencode(owner).hex(),
                                      "start": start, "end": len(shapes) - 1,
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
        assert failed["line"] == {"file": str(path), "file_bytes": os.fsencode(path).hex(),
                                   "start": start, "end": start + 31,
                                   "line": 8, "column": 1, "end_line": 8, "end_column": 32}
        defect = by_name["Main.test.3"]["defect"]
        start = shapes.index("base / value")
        assert defect["reason"] == "dividing by zero"
        assert defect["span"] == {"file": str(owner), "file_bytes": os.fsencode(owner).hex(),
                                   "start": start, "end": start + 12,
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
                ("file", "file_bytes", "start", "end", "line", "column", "end_line", "end_column")} == {
                    "file": str(path), "file_bytes": os.fsencode(path).hex(),
                    "start": 36, "end": 41, "line": 2, "column": 25,
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
        return {"file": str(path), "file_bytes": os.fsencode(path).hex(),
                "start": start, "end": start + len(row),
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
