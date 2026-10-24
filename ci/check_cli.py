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

        # Independent SipHash-1-3/SplitMix calculation: after the five Int
        # boundaries these promises first generate 9 and -9, respectively.
        source = 'module Main\nfn seeded(value: Int) : Int\n'
        for sentence in ('first stream', 'second stream'):
            source += (f'promise "{sentence}"\nresult == value\n'
                       'allows value: 3, result: 3\nforbids value: 3, result: 4\nend\n')
        source += ('if value == 9 or value == -9\nstop "random input"\nend\nvalue\nend\n'
                   'fn main()\nstop "main ran"\nend\n')
        path.write_text(source)
        for _ in range(2):
            items = records(run(binary, ['test', '--json', '--calls', '5', str(path)], 0), 'test')
            assert [r['outcome'] for r in items[1:-1]] == ['passed', 'passed']
            items = records(run(binary, ['test', '--json', '--calls', '6', str(path)], 1), 'test')
            assert [r['input'] for r in items[1:-1]] == [
                [{'name': 'value', 'value': '9'}], [{'name': 'value', 'value': '-9'}]]
            assert [r['outcome'] for r in items[1:-1]] == ['stopped', 'stopped']

        # Independently calculated unique Int at ordinal 200 (trial 201).
        # This distinguishes the default 200 from an off-by-one/default drift.
        target = '-1459837627065130'
        path.write_text('module Main\nfn capped(value: Int) : Int\n'
                        'promise "keeps input"\nresult == value\n'
                        'allows value: 3, result: 3\nforbids value: 3, result: 4\nend\n'
                        f'if value == {target}\nstop "trial 201"\nend\nvalue\nend\nfn main()\nend\n')
        items = records(run(binary, ['test', '--json', str(path)], 0), 'test')
        assert items[1]['outcome'] == 'passed'
        items = records(run(binary, ['test', '--json', '--calls', '201', str(path)], 1), 'test')
        assert items[1]['outcome'] == 'stopped' and items[1]['input'] == [
            {'name': 'value', 'value': target}]


def promise_boundaries(binary):
    minimum, maximum = '-9223372036854775808', '9223372036854775807'
    cases = [
        ('unit', 'Unit', 'Unit', 'true', 'Unit'),
        ('boolean', 'Bool', 'false', 'value', 'true'),
        ('byte', 'Byte', '0', 'value == 255', '255'),
        ('integer', 'Int', '13', f'value == {minimum}', minimum),
        ('float', 'Float', '1.0', 'value > 1.0', '1.7976931348623157e308'),
        ('character', 'Char', "'a'", "value == 'é'", "'é'"),
        ('decimal', 'Decimal', '0.0', 'value > 1.0', '170141183460469231731.687303715884105727'),
        ('range', 'Range', '0..3', f'value == ({minimum}..{maximum})', f'{minimum}..{maximum}'),
        ('text', 'Text', '"ok"', 'value == "é"', '"é"'),
        ('bigint', 'BigInt', '3', 'value > 0x10000000000000000', str((1 << 1024) - 1)),
    ]
    source = 'module Main\n'
    for name, type_, example, predicate, _ in cases:
        parameter = '_value' if name == 'unit' else 'value'
        source += (f'fn {name}({parameter}: {type_}) : Int\n'
                   f'promise "scalar boundary {name}"\nresult == 0\n'
                   f'allows {parameter}: {example}, result: 0\n'
                   f'forbids {parameter}: {example}, result: 1\nend\n'
                   f'if {predicate}\nstop "boundary {name}"\nend\n0\nend\n')
    source += 'test "after all boundaries"\nassert true\nend\nfn main()\nstop "main ran"\nend\n'
    with tempfile.TemporaryDirectory(prefix='mo-promise-boundaries-') as directory:
        path = pathlib.Path(directory) / 'main.mo'
        path.write_text(source)
        items = records(run(binary, ['test', '--json', '--calls', '6', str(path)], 1), 'test')
        for result, (name, _, _, _, expected) in zip(items[1:-2], cases):
            parameter = '_value' if name == 'unit' else 'value'
            assert result['name'] == f'Main.{name}.promise.1' and result['outcome'] == 'stopped'
            assert result['input'] == [{'name': parameter, 'value': expected}]
            assert result['defect']['variables'] == [{'name': parameter, 'value': expected}]
            defect = result['defect']
            assert defect['reason'] == 'stop' and defect['detail'] == f'boundary {name}'
            start = source.encode().index(f'stop "boundary {name}"'.encode())
            assert (defect['span']['start'], defect['span']['end']) == (
                start, start + len(f'stop "boundary {name}"'))
            assert [f['function'] for f in defect['stack']] == [f'Main.{name}']
        assert items[4]['extreme'] == {'name': 'value', 'description': "Int's smallest value"}
        assert items[7]['extreme'] == {'name': 'value', 'description': "Decimal's largest value"}
        assert items[8]['extreme'] == {'name': 'value', 'description': "Int's smallest value"}
        assert items[-2]['outcome'] == 'passed'
        assert items[-1] == {'record': 'totals', 'errors': 0, 'dropped': 0,
                             'passed': 1, 'failed': 0, 'stopped': 10, 'status': 1}
        text = run(binary, ['test', '--calls', '6', str(path)], 1)
        assert not text.stderr and text.stdout.endswith(b'1 passed, 0 failed, 10 stopped\n')
        assert b"Int's smallest value" in text.stdout and b"Decimal's largest value" in text.stdout
        assert b'return a failure' in text.stdout and b'keeps its values in range' in text.stdout


def promise_deadlines(binary):
    clock = binary.parent / 'tests/test_deadline.test'
    minimum = '-9223372036854775808'
    loop = 'var spin = 0\nwhile spin == 0\nspin = 0\nend\n'
    tail = 'test "after promise"\nassert true\nend\nfn main()\nstop "main ran"\nend\n'
    with tempfile.TemporaryDirectory(prefix='mo-promise-deadlines-') as directory:
        path = pathlib.Path(directory) / 'main.mo'

        def observe(source, mode, seconds='1'):
            path.write_text(source + tail)
            return records(run(clock, [mode, 'test', '--json', '--seconds', seconds, str(path)], 1), 'test')

        # Neither a nonreturning subject nor a nonreturning condition invents
        # a promise outcome. Only the following independent test is counted.
        subject = ('module Main\nfn probe(value: Int) : Int\n'
                   'promise "returns input"\nresult == value\n'
                   'allows value: 13, result: 13\nforbids value: 13, result: 14\nend\n'
                   f'if value != 13\n{loop}end\nvalue\nend\n')
        condition = ('module Main\nfn verdict(value: Int, answer: Int) : Bool\n'
                     f'if value != 13 and answer == value\n{loop}end\nvalue == answer\nend\n'
                     'fn probe(value: Int) : Int\npromise "returns input"\nverdict(value, result)\n'
                     'allows value: 13, result: 13\nforbids value: 13, result: 14\nend\nvalue\nend\n')
        for source in (subject, condition):
            for mode in ('expiry', 'failure', 'start-failure'):
                items = observe(source, mode, '2' if mode == 'failure' else '1')
                assert [(r['name'], r['outcome']) for r in items if r['record'] == 'result'] == [
                    ('Main.test.1', 'passed')]
                diagnostic = [r for r in items if r['record'] == 'diagnostic']
                assert len(diagnostic) == 1 and diagnostic[0]['code'] == 'E1404'
                assert ('clock could not be read' in diagnostic[0]['headline']) == (mode != 'expiry')
                assert items[-1] == {'record': 'totals', 'errors': 1, 'dropped': 0,
                                     'passed': 1, 'failed': 0, 'stopped': 0, 'status': 1}
        # Actual CLOCK_MONOTONIC cancellation, with run()'s 30-second safety
        # watchdog. No fragile wall-duration threshold or instruction budget.
        path.write_text(subject + tail)
        items = records(run(binary, ['test', '--json', '--seconds', '1', str(path)], 1), 'test')
        assert [r['code'] for r in items if r['record'] == 'diagnostic'] == ['E1404']
        assert items[-1]['passed'] == 1 and items[-1]['failed'] == items[-1]['stopped'] == 0

        # Finite calls each fit individually. Their cumulative work must not
        # get a fresh deadline per call, condition or completed trial.
        work = 'var n = 0\nwhile n < 37\nn += 1\nend\n'
        finite = ('module Main\nfn verdict(value: Int, answer: Int) : Bool\n' + work +
                  'value == answer\nend\nfn probe(value: Int) : Int\n'
                  'promise "returns input"\nverdict(value, result)\n'
                  'allows value: 13, result: 13\nforbids value: 13, result: 14\nend\n' + work +
                  'value\nend\n')
        items = observe(finite, 'expiry')
        assert [r['code'] for r in items if r['record'] == 'diagnostic'] == ['E1404']
        assert items[-1]['passed'] == 1 and items[-1]['failed'] == items[-1]['stopped'] == 0

        # The old confirmed result is -31; the next candidate would return
        # -47. Cancellation during its subject OR condition must keep all three
        # old fields, whereas a genuine stopped condition reports the new input.
        for where in ('subject', 'condition', 'stop'):
            operation = 'stop "candidate stopped"\n' if where == 'stop' else loop
            parameter = '_value' if where == 'subject' else 'value'
            helper = (f'fn verdict({parameter}: Int, answer: Int) : Bool\n' +
                      (f'if value < -1 and value != {minimum}\n{operation}end\n'
                       if where != 'subject' else '') + 'answer >= 0\nend\n')
            source = ('module Main\n' + helper + 'fn probe(value: Int) : Int\n'
                      'promise "retains confirmed failure"\nresult <= 0\nverdict(value, result)\n'
                      f'allows value: 13, result: 0\nforbids value: {minimum}, result: -31\nend\n' +
                      (f'if value < -1 and value != {minimum}\n{operation}end\n'
                       if where == 'subject' else '') +
                      f'if value == {minimum}: -31 else if value < -1: -47 else: 0\nend\n')
            for mode in (('fixed',) if where == 'stop' else ('expiry', 'failure')):
                items = observe(source, mode, '2' if mode == 'failure' else '1')
                result = items[1]
                assert items[2]['name'] == 'Main.test.1' and items[2]['outcome'] == 'passed'
                if where == 'stop':
                    assert result['outcome'] == 'stopped' and 'result' not in result and 'line' not in result
                    assert result['input'] == [{'name': 'value', 'value': '-4611686018427387904'}]
                    assert result['defect']['detail'] == 'candidate stopped'
                    assert [f['function'] for f in result['defect']['stack']] == [
                        'Main.verdict', 'Main.probe.promise.1']
                    assert result['defect']['variables'] == [
                        {'name': 'value', 'value': '-4611686018427387904'}, {'name': 'answer', 'value': '-47'}]
                    assert items[-1]['stopped'] == 1 and items[-1]['failed'] == 0
                else:
                    assert result['outcome'] == 'failed' and result['calls'] == 6
                    assert result['input'] == [{'name': 'value', 'value': minimum}]
                    assert result['result'] == '-31'
                    start = source.encode().index(b'verdict(value, result)')
                    assert (result['line']['start'], result['line']['end']) == (start, start + 22)
                    assert items[-1]['failed'] == 1 and items[-1]['stopped'] == 0
                diagnostics = [r['code'] for r in items if r['record'] == 'diagnostic']
                assert diagnostics == (['E1404'] if mode == 'failure' else [])

        # Finite shrink candidates fit individually but not cumulatively.
        # The exact interruption point is not a language contract: it depends
        # on bytecode instruction count. The true minimum is -2; expiry must
        # retain an earlier confirmed tuple, not reset time until reaching -2.
        source = ('module Main\nfn probe(value: Int) : Int\n'
                  'promise "nonnegative result"\nresult >= 0\n'
                  f'allows value: 13, result: 0\nforbids value: {minimum}, result: {minimum}\nend\n'
                  f'if value < -1 and value != {minimum}\n{work}end\n'
                  'if value < -1: value else: 0\nend\n')
        items = observe(source, 'expiry')
        result = items[1]
        assert result['outcome'] == 'failed' and result['microseconds'] == 1000000
        assert int(result['input'][0]['value']) < -2
        assert result['result'] == result['input'][0]['value']
        assert items[-1]['errors'] == 0 and items[-1]['failed'] == 1 and items[-1]['passed'] == 1


def promise_shrinking(binary):
    clock = binary.parent / 'tests/test_deadline.test'
    tail = 'test "after promise"\nassert true\nend\nfn main()\nstop "main ran"\nend\n'
    with tempfile.TemporaryDirectory(prefix='mo-promise-shrinking-') as directory:
        path = pathlib.Path(directory) / 'main.mo'
        source = ('module Main\nfn probe(value: Int) : Int\n'
                  'promise "nonnegative answer"\nresult >= 0\n'
                  'allows value: 3, result: 4\nforbids value: -2, result: -1\nend\n'
                  'value + 1\nend\n')
        path.write_text(source + tail)
        items = records(run(clock, ['fixed', 'test', '--json', '--calls', '4', str(path)], 1), 'test')
        assert items[1]['input'] == [{'name': 'value', 'value': '-2'}]
        assert items[1]['result'] == '-1' and 4 < items[1]['calls'] <= 204
        assert items[-1]['failed'] == items[-1]['passed'] == 1 and items[-1]['errors'] == 0
        text = run(binary, ['test', '--calls', '4', str(path)], 1)
        assert not text.stderr and b'input value = -2\n  result: -1\n' in text.stdout
        assert b'this promise condition was false' in text.stdout

        # This takes more than 200 candidates to minimize. The search cap is
        # four subject calls; shrinking gets a separate 200, not four or 196.
        threshold = '0x1' + '0' * 128
        source = ('module Main\nfn probe(value: BigInt) : BigInt\n'
                  f'promise "below threshold"\nresult < {threshold}\n'
                  f'allows value: 3, result: 3\nforbids value: {threshold}, result: {threshold}\nend\n'
                  'value\nend\n')
        path.write_text(source + tail)
        for calls, status in (('3', 0), ('4', 1)):
            items = records(run(clock, ['fixed', 'test', '--json', '--calls', calls, str(path)], status), 'test')
            result = items[1]
            if status == 0:
                assert result['outcome'] == 'passed'
            else:
                assert result['outcome'] == 'failed' and result['calls'] == 204
                assert (1 << 512) < int(result['input'][0]['value']) < (1 << 1024) - 1
                assert result['result'] == result['input'][0]['value']

        # Owned input and structural result survive candidate/process teardown.
        # Reporting must never execute the stopping source Text customizer.
        source = ('module Main\nstruct Box\nvalue: Text\nend\n'
                  'fn to_text(_value: Box) : Text\nstop "renderer called customizer"\nend\n'
                  'fn probe(value: Text) : Box\npromise "avoids one text"\nresult.value != "é"\n'
                  'allows value: "ok", result: Box(value: "ok")\n'
                  'forbids value: "é", result: Box(value: "é")\nend\nBox(value: value)\nend\n')
        path.write_text(source + tail)
        items = records(run(binary, ['test', '--json', str(path)], 1), 'test')
        assert items[1]['input'] == [{'name': 'value', 'value': '"é"'}]
        assert items[1]['result'] == 'Box(value: "é")' and items[1]['calls'] == 4
        start = source.encode().index('result.value != "é"'.encode())
        assert (items[1]['line']['start'], items[1]['line']['end']) == (start, start + 20)
        assert items[-1]['stopped'] == 0 and items[-1]['failed'] == items[-1]['passed'] == 1

        # Complete structural values include their quotes in the 4096-byte cap.
        # A refused record is never truncated or emitted as invalid JSON.
        for length in (4094, 4095):
            path.write_text('module Main\nfn probe() : Text\npromise "small result"\n'
                            'result == "ok"\nallows result: "ok"\nforbids result: "bad"\nend\n"' +
                            'x' * length + '"\nend\n' + tail)
            items = records(run(binary, ['test', '--json', str(path)], 1), 'test')
            if length == 4094:
                assert items[1]['result'] == '"' + 'x' * length + '"'
                assert items[-1]['errors'] == 0 and items[-1]['passed'] == 1
            else:
                assert len(items) == 3 and items[1]['code'] == 'E1404'
                assert items[-1]['errors'] == 1 and items[-1]['passed'] == 0
            assert items[-1]['failed'] == 1 and items[-1]['stopped'] == 0


def generic_promises(binary):
    fixture = root / 'tests/lower-source/generic-subject/main.mo'
    items = records(run(binary, ['test', '--json', str(fixture)], 0), 'test')
    promises = [r for r in items[1:-1] if r['kind'] == 'promise']
    assert {r['instance'] for r in promises} == {
        'Main::Main.swap(Int, Text).promise.1',
        'Main::Main.swap(Bool, Decimal).promise.1',
    }
    assert len(promises) == 2 and all(r['outcome'] == 'passed' for r in promises)
    assert items[-1]['passed'] == 6 and items[-1]['errors'] == 0
    with tempfile.TemporaryDirectory(prefix='mo-generic-promises-') as directory:
        path = pathlib.Path(directory) / 'main.mo'
        path.write_text('module Main\nfn probe(n: Int, _value: List(T)) : Int\n'
                        'promise "nonnegative answer"\nresult >= 0\n'
                        'allows _value: [13], n: 3, result: 4\n'
                        'forbids _value: [13], n: -2, result: -1\nend\nn + 1\nend\n'
                        'fn main() : Int\n_text = probe(3, ["text"])\nprobe(3, [true])\nend\n')
        clock = binary.parent / 'tests/test_deadline.test'
        items = records(run(clock, ['fixed', 'test', '--json', '--calls', '4', str(path)], 1), 'test')
        results = items[1:-1]
        assert {r['instance'] for r in results} == {
            f'Main::Main.probe({t}).promise.1' for t in ('Int', 'Text', 'Bool')}
        assert len(results) == 3
        for result in results:
            assert result['outcome'] == 'failed' and result['result'] == '-1'
            assert result['input'] == [{'name': 'n', 'value': '-2'},
                                       {'name': '_value', 'value': '[]'}]
            assert 4 < result['calls'] <= 204
        assert items[-1]['failed'] == 3 and items[-1]['errors'] == 0
        text = run(binary, ['test', '--calls', '4', str(path)], 1)
        assert not text.stderr
        for t in ('Int', 'Text', 'Bool'):
            assert f'instance: Main::Main.probe({t}).promise.1'.encode() in text.stdout

        (path.parent / 'shapes.mo').write_text(
            'module Shapes\nexpose Box(..)\nstruct Box(T)\nvalue: T\nend\n')
        (path.parent / 'holds.mo').write_text(
            'module Holds\nexpose keep\nfn keep(value: T) : T where T: Equal\n'
            'promise "keeps input"\nresult == value\n'
            'allows value: 13, result: 13\nforbids value: 13, result: 14\nend\nvalue\nend\n')
        path.write_text('module Main\nuse Holds\nuse Shapes\n'
                        'fn forward(value: T) : T where T: Equal\nHolds.keep(value)\nend\n'
                        'fn main()\n_box = forward(Shapes.Box(value: "text"))\n'
                        '_list = Holds.keep([true])\n_again = Holds.keep([false])\nend\n')
        items = records(run(binary, ['test', '--json', '--only', 'Holds.keep.promise.1',
                                     '--only', 'Holds.keep.promise.1', '--in', 'Holds', str(path)], 0), 'test')
        assert {r['instance'] for r in items[1:-1]} == {
            'Holds::Holds.keep(Int).promise.1',
            'Main::Holds.keep(Shapes.Box(Text)).promise.1',
            'Main::Holds.keep(List(Bool)).promise.1',
        }
        assert items[-1]['passed'] == 3 and items[-1]['errors'] == 0
        listed = records(run(binary, ['test', '--json', '--list', str(path)], 0), 'test')
        assert [r['name'] for r in listed[1:-1]] == ['Holds.keep.promise.1']

        path.write_text('module Main\nseed = crash()\nfn crash() : Int\nstop "initializer ran"\nend\n'
                        'test "first observation"\nassert true\nend\n'
                        'fn probe(_value: T) : Int\npromise "seven"\nresult == 7\n'
                        'allows _value: 13, result: 7\nforbids _value: 13, result: 8\nend\n7\nend\n'
                        'fn identity(value: Int) : Int\nvalue\nend\n'
                        'fn main() : Int\nprobe(identity)\nend\n')
        items = records(run(binary, ['test', '--json', str(path)], 1), 'test')
        assert len(items) == 3 and items[1]['code'] == 'E1400'
        assert items[1]['primary']['label'] == 'Main.probe.promise.1'
        assert all(items[-1][key] == 0 for key in ('passed', 'failed', 'stopped'))
        items = records(run(binary, ['test', '--json', '--only', 'Main.test.1', str(path)], 1), 'test')
        assert items[1]['code'] == 'E1406'


def fake_capability_promises(binary):
    tail = 'fn main()\nend\n'
    with tempfile.TemporaryDirectory(prefix='mo-fake-promises-') as directory:
        path = pathlib.Path(directory) / 'main.mo'

        # Every capability a promise takes is a generated fake (reference 12.4).
        path.write_text(
            'module Main\n'
            'fn all(files: Files, console: Console, clock: Clock, args: Args) : Int\n'
            'promise "holds for every kind of fake"\nresult == 0\n'
            'allows files: Files.in_memory({}), console: Console.fake(), clock: Clock.fixed(t0), '
            'args: Args.of([]), result: 0\n'
            'forbids files: Files.in_memory({}), console: Console.fake(), clock: Clock.fixed(t0), '
            'args: Args.of([]), result: 1\nend\n'
            '_names = files.list(".")\n_line = console.read_line()\n_now = clock.now()\n'
            '_words = args.list()\n0\nend\n'
            't0 = Time.parse("1970-01-01T00:00:00Z") or stop "the time is written correctly"\n' + tail)
        items = records(run(binary, ['test', '--json', str(path)], 0), 'test')
        assert [(r['name'], r['outcome']) for r in items[1:-1]] == [
            ('Main.all.promise.1', 'passed')]

        # The first fake with a file in it fails this promise, on the second call, and the
        # report shows the capability by its type rather than by contents.
        path.write_text(
            'module Main\nfn count(files: ReadFiles) : Int\n'
            'promise "no files at all"\nresult == 0\n'
            'allows files: Files.in_memory({}).read_only, result: 0\n'
            'forbids files: Files.in_memory({}).read_only, result: 1\nend\n'
            'case files.list(".")\nOk(names): names.size\nError(_e): 0\nend\nend\n' + tail)
        items = records(run(binary, ['test', '--json', str(path)], 1), 'test')
        assert items[1]['outcome'] == 'failed' and items[1]['calls'] == 2
        assert items[1]['input'] == [{'name': 'files', 'value': '<ReadFiles>'}]
        assert items[1]['result'] == '1'
        again = records(run(binary, ['test', '--json', str(path)], 1), 'test')
        for key in ('input', 'result', 'calls', 'line'):
            assert again[1][key] == items[1][key]

        # Shrinking copies the fake along with the input it shrinks.
        path.write_text(
            'module Main\nfn pick(files: ReadFiles, limit: Int) : Int\n'
            'promise "stays small"\nresult <= 5\n'
            'allows files: Files.in_memory({}).read_only, limit: 1, result: 1\n'
            'forbids files: Files.in_memory({}).read_only, limit: 1, result: 6\nend\n'
            'case files.list(".")\nOk(_names): limit\nError(_e): limit\nend\nend\n' + tail)
        items = records(run(binary, ['test', '--json', str(path)], 1), 'test')
        assert items[1]['outcome'] == 'failed'
        assert items[1]['input'] == [{'name': 'files', 'value': '<ReadFiles>'},
                                     {'name': 'limit', 'value': '6'}]
        assert items[-1]['errors'] == 0

    # examples/notes-tool.mo asks for a ReadFiles in its promise.
    root = pathlib.Path(__file__).resolve().parent.parent
    items = records(run(binary, ['test', '--json', str(root / 'examples/notes-tool.mo')], 0), 'test')
    assert [(r['name'], r['outcome']) for r in items[1:-1]] == [
        ('NotesTool.search_notes.promise.1', 'passed'), ('NotesTool.test.1', 'passed'),
        ('NotesTool.test.2', 'passed'), ('NotesTool.test.3', 'passed')]


def hidden_promises(binary):
    # Reference 12.2: a type exposed without its insides is built only through its module, from
    # an exposed constant or a function that returns it, then up to three calls that take it.
    tail = 'test "after promise"\nassert true\nend\nfn main()\nend\n'
    counter = ('module Main\nexpose Counter, zero, bump, count\n'
               'struct Counter\nvalue: Int\nend\n'
               'zero = Counter(value: 0)\nhidden = Counter(value: -1)\n'
               '## A tuple result holds the counter; a negative step stops.\n'
               'fn bump(counter: Counter, by: Int) : (Counter, Bool)\n'
               'if by < 0\nstop "a negative step"\nend\n'
               '(Counter(value: counter.value + by % 10), by % 10 != 0)\nend\n'
               'fn count(counter: Counter) : Int\n')
    with tempfile.TemporaryDirectory(prefix='mo-hidden-promises-') as directory:
        path = pathlib.Path(directory) / 'main.mo'
        # Never the private constant, never a stopped step: every counter is 0 to 27.
        path.write_text(counter + 'promise "in range"\nresult >= 0 and result <= 27\n'
                        'allows counter: zero, result: 0\nforbids counter: zero, result: 28\n'
                        'end\ncounter.value\nend\n' + tail)
        items = records(run(binary, ['test', '--json', str(path)], 0), 'test')
        assert [(r['name'], r['outcome']) for r in items[1:-1]] == [
            ('Main.count.promise.1', 'passed'), ('Main.test.1', 'passed')]
        # Reaching 15 takes two steps after the constant, so the steps chain. Built values are
        # reported as they are, not shrunk, and the same on every run.
        path.write_text(counter + 'promise "stays small"\nresult < 15\n'
                        'allows counter: zero, result: 0\nforbids counter: zero, result: 15\n'
                        'end\ncounter.value\nend\n' + tail)
        items = records(run(binary, ['test', '--json', str(path)], 1), 'test')
        failure = items[1]
        assert failure['outcome'] == 'failed', failure
        value = failure['input'][0]['value']
        assert value.startswith('Counter(value: ') and 15 <= int(value[15:-1]) <= 27, value
        again = records(run(binary, ['test', '--json', str(path)], 1), 'test')
        for key in ('input', 'result', 'calls', 'line'):
            assert again[1][key] == failure[key]

        # An Option start counts only when it returns Some, and steps may come from Result.
        path.write_text('module Main\nexpose Counter, start, add\nstruct Counter\nvalue: Int\nend\n'
                        'fn start(value: Int) : Option(Counter)\n'
                        'if value >= 1 and value <= 100: Some(Counter(value: value)) else: None\nend\n'
                        'fn add(counter: Counter, more: Int) : Result(Counter, Text)\n'
                        'if more >= 1 and more <= 100: Ok(Counter(value: counter.value + more)) '
                        'else: Error("no")\nend\n'
                        'fn count(counter: Counter) : Int\npromise "within what start and add allow"\n'
                        'result >= 1 and result <= 400\n'
                        'allows counter: Counter(value: 1), result: 1\n'
                        'forbids counter: Counter(value: 1), result: 0\nend\ncounter.value\nend\n' + tail)
        items = records(run(binary, ['test', '--json', str(path)], 0), 'test')
        assert items[1]['outcome'] == 'passed' and items[-1]['status'] == 0

        # No way to start one: steps alone, a start that needs a capability, or a private
        # constant. The whole selection is refused before it runs.
        for source in ('module Main\nexpose Counter, bump\nstruct Counter\nvalue: Int\nend\n'
                       'fn bump(counter: Counter) : Counter\nCounter(value: counter.value + 1)\nend\n',
                       'module Main\nexpose Counter\nstruct Counter\nvalue: Int\nend\n'
                       'zero = Counter(value: 0)\n',
                       'module Main\nexpose Counter, open\nstruct Counter\nvalue: Int\nend\n'
                       'fn open(console: Console) : Counter\nconsole.print("x")\nCounter(value: 0)\nend\n'):
            path.write_text(source + 'fn count(counter: Counter) : Int\npromise "anything"\n'
                            'result == counter.value\n'
                            'allows counter: Counter(value: 1), result: 1\n'
                            'forbids counter: Counter(value: 1), result: 2\nend\ncounter.value\nend\n' + tail)
            items = records(run(binary, ['test', '--json', str(path)], 1), 'test')
            assert len(items) == 3 and items[1]['code'] == 'E1400', items
            assert items[-1]['passed'] == 0

        # A start exists, but a built value can't be copied into a trial: a capability or a
        # function among its insides. Refused before the run too.
        for inside, value in (('enum Handle\nEmpty\nLive(Console)\nend\n', 'Empty'),
                              ('struct Handle\nf: fn(Int) : Int\nend\n', 'Handle(f: same)')):
            path.write_text('module Main\nexpose Handle, empty, count\n' + inside +
                            'fn same(x: Int) : Int\nx\nend\n' + f'empty = {value}\n'
                            'fn count(_handle: Handle) : Int\npromise "zero"\nresult == 0\n'
                            'allows _handle: empty, result: 0\nforbids _handle: empty, result: 1\n'
                            'end\n0\nend\n' + tail)
            items = records(run(binary, ['test', '--json', str(path)], 1), 'test')
            assert len(items) == 3 and items[1]['code'] == 'E1400', items

        # Boundary 0 is the start alone, boundary 1 one step and boundary 2 three.
        path.write_text('module Main\nexpose Counter, zero, bump\nstruct Counter\nvalue: Int\nend\n'
                        'zero = Counter(value: 0)\n'
                        'fn bump(counter: Counter) : Counter\nCounter(value: counter.value + 1)\nend\n'
                        'fn count(counter: Counter) : Int\npromise "under three"\nresult < 3\n'
                        'allows counter: zero, result: 0\nforbids counter: zero, result: 3\nend\n'
                        'counter.value\nend\n' + tail)
        items = records(run(binary, ['test', '--json', str(path)], 1), 'test')
        assert items[1]['outcome'] == 'failed' and items[1]['calls'] == 3, items[1]
        assert items[1]['input'] == [{'name': 'counter', 'value': 'Counter(value: 3)'}]

        # A builder may take other hidden types, each built by its own factory first: a Stamp
        # needs a Ticket, which needs a Pool. Every stamp is a multiple of 10 from 10 to 50 only
        # when each one comes through its builders, never from its insides.
        chain = ('struct Pool\nsize: Int\nend\nstruct Ticket\nseat: Int\nend\n'
                 'struct Stamp\nseat: Int\nend\n'
                 'fn open(size: Int) : Option(Pool)\n'
                 'if size >= 1 and size <= 5: Some(Pool(size: size)) else: None\nend\n'
                 'fn issue(pool: Pool, seat: Int) : Option(Ticket)\n'
                 'if seat >= 1 and seat <= pool.size: Some(Ticket(seat: seat)) else: None\nend\n'
                 'fn stamp(ticket: Ticket) : Stamp\nStamp(seat: ticket.seat * 10)\nend\n')
        probe = ('fn seat_of(stamp: Stamp) : Int\npromise "a seat a pool had"\n'
                 'result % 10 == 0 and result >= 10 and result <= 50\n'
                 'allows stamp: Stamp(seat: 10), result: 10\n'
                 'forbids stamp: Stamp(seat: 10), result: 7\nend\nstamp.seat\nend\n')
        exposed = 'module Main\nexpose Pool, Ticket, Stamp, open, issue, stamp'
        path.write_text(exposed + '\n' + chain + probe + tail)
        items = records(run(binary, ['test', '--json', str(path)], 0), 'test')
        assert [(r['name'], r['outcome']) for r in items[1:-1]] == [
            ('Main.seat_of.promise.1', 'passed'), ('Main.test.1', 'passed')], items
        # That is MO_CLI_BUILDER_LEVELS levels; a fourth, a Badge from a Stamp, is refused, and
        # so is a cycle: an A only from a B and a B only from an A.
        badge = ('struct Badge\nseat: Int\nend\n'
                 'fn pin(stamp: Stamp) : Badge\nBadge(seat: stamp.seat)\nend\n'
                 'fn seat_of(badge: Badge) : Int\npromise "positive"\nresult > 0\n'
                 'allows badge: Badge(seat: 10), result: 10\n'
                 'forbids badge: Badge(seat: 10), result: 0\nend\nbadge.seat\nend\n')
        cycle = ('module Main\nexpose A, B, a, b\nstruct A\nn: Int\nend\nstruct B\nn: Int\nend\n'
                 'fn a(b: B) : A\nA(n: b.n)\nend\nfn b(a: A) : B\nB(n: a.n)\nend\n'
                 'fn n_of(a: A) : Int\npromise "anything"\nresult == a.n\n'
                 'allows a: A(n: 1), result: 1\nforbids a: A(n: 1), result: 2\nend\na.n\nend\n')
        for source in (exposed + ', Badge, pin\n' + chain + badge, cycle):
            path.write_text(source + tail)
            items = records(run(binary, ['test', '--json', str(path)], 1), 'test')
            assert len(items) == 3 and items[1]['code'] == 'E1400', items
            assert items[-1]['passed'] == 0

        # A value comes with the hidden value its call returned beside it (D284). make returns
        # an A and a B together, and each one's build keeps the other as its companion; taking
        # an A's companion must leave b alone, or the swap pairs them crosswise, unrelated again.
        # Boundary Ints (0, 1, -1 and the extremes) collide anyway, so the promise excuses them.
        # The 9 boundary tuples (3 per parameter) and the first random trial keep their own
        # values; the second random trial, call 11, takes companions and so holds a related pair.
        path.write_text('module Main\nexpose A, B, make\nstruct A\nn: Int\nm: Int\nend\n'
                        'struct B\nn: Int\nm: Int\nend\n'
                        'fn make(n: Int, m: Int) : (A, B)\n(A(n: n, m: m), B(n: n, m: m))\nend\n'
                        'fn apart(a: A, b: B) : Bool\npromise "never from the same call"\n'
                        'result or edge?(a.n)\n'
                        'allows a: A(n: 1, m: 2), b: B(n: 2, m: 2), result: true\n'
                        'forbids a: A(n: 5, m: 2), b: B(n: 5, m: 2), result: false\nend\n'
                        'a.n != b.n or a.m != b.m\nend\n'
                        'fn edge?(n: Int) : Bool\n'
                        'n >= -1 and n <= 1 or n < -1000000000000000000 or n > 1000000000000000000'
                        '\nend\n' + tail)
        items = records(run(binary, ['test', '--json', str(path)], 1), 'test')
        assert items[1]['outcome'] == 'failed' and items[1]['calls'] == 11, items[1]
        a, b = (entry['value'] for entry in items[1]['input'])
        assert a[1:] == b[1:] and a[0] == 'A' and b[0] == 'B', items[1]

        # Random values fall back to the boundary order's full search: this start counts only on
        # its 45th boundary tuple (true, Int's smallest, Int's largest), past the 32 ordered tries a
        # random value once had and out of reach of random Ints.
        path.write_text('module Main\nexpose Pool, open\nstruct Pool\nsize: Int\nend\n'
                        'fn open(flag: Bool, a: Int, b: Int) : Option(Pool)\n'
                        'if flag and a < -1000000000000000000 and b > 1000000000000000000\n'
                        'Some(Pool(size: 1))\nelse\nNone\nend\nend\n'
                        'fn size_of(pool: Pool) : Int\npromise "one"\nresult == 1\n'
                        'allows pool: Pool(size: 1), result: 1\n'
                        'forbids pool: Pool(size: 1), result: 2\nend\npool.size\nend\n' + tail)
        items = records(run(binary, ['test', '--json', str(path)], 0), 'test')
        assert items[1]['outcome'] == 'passed' and items[-1]['errors'] == 0, items

        # Each hidden type is built by its own module, whatever order the image lists them in;
        # Alpha's generic Box(Int) comes after Beta's B among the types.
        (path.parent / 'alpha.mo').write_text(
            'module Alpha\nexpose Box, make_box, get_box\nstruct Box(T)\nvalue: T\nend\n'
            'fn make_box(value: Int) : Box(Int)\nBox(value: value)\nend\n'
            'fn get_box(b: Box(Int)) : Int\nb.value\nend\n')
        (path.parent / 'beta.mo').write_text(
            'module Beta\nexpose B, make_b, get_b\nstruct B\nvalue: Int\nend\n'
            'fn make_b(value: Int) : B\nB(value: value)\nend\nfn get_b(b: B) : Int\nb.value\nend\n')
        path.write_text('module Main\nuse Alpha\nuse Beta\nfn probe(b: Beta.B) : Int\n'
                        'promise "anything"\nresult == 0\n'
                        'allows b: Beta.make_b(0), result: 0\nforbids b: Beta.make_b(0), result: 1\n'
                        'end\nBeta.get_b(b) * 0 + Alpha.get_box(Alpha.make_box(1)) * 0\nend\n' + tail)
        items = records(run(binary, ['test', '--json', str(path)], 0), 'test')
        assert items[1]['outcome'] == 'passed' and items[-1]['errors'] == 0, items

        # A step that never returns ends the search at the promise's deadline, between trials.
        path.write_text('module Main\nexpose Counter, zero, spin\nstruct Counter\nvalue: Int\nend\n'
                        'zero = Counter(value: 0)\n'
                        'fn spin(counter: Counter) : Counter\nvar n = 0\nwhile n == 0\nn = 0\nend\n'
                        'counter\nend\n'
                        'fn count(counter: Counter) : Int\npromise "zero"\nresult == 0\n'
                        'allows counter: zero, result: 0\nforbids counter: zero, result: 1\nend\n'
                        'counter.value\nend\n' + tail)
        items = records(run(binary, ['test', '--json', '--seconds', '1', str(path)], 0), 'test')
        assert items[1]['outcome'] == 'passed' and items[-1]['errors'] == 0, items

    # The samples whose promises take Store, Limiter, Queue and Account.
    root = pathlib.Path(__file__).resolve().parent.parent
    for sample in ('account', 'job-queue', 'kv', 'rate-limit'):
        items = records(run(binary, ['test', '--json', str(root / f'examples/{sample}.mo')], 0), 'test')
        promises = [r for r in items if r.get('kind') == 'promise']
        assert promises and all(r['outcome'] == 'passed' for r in promises), (sample, items)
        assert items[-1]['failed'] == items[-1]['stopped'] == items[-1]['errors'] == 0

    # MO-121's job queue: its opaque Lease comes only from lease, which takes an opaque Queue.
    with tempfile.TemporaryDirectory(prefix='mo-hidden-chain-') as directory:
        answer = pathlib.Path(directory) / 'answers/job-queue.mo'
        answer.parent.mkdir()
        answer.write_bytes((root / 'learnability/final/gpt-6.1-sol/job-queue.mo').read_bytes())
        for only in ([], ['--only', 'Answers.JobQueue.acknowledge.promise.1'],
                     ['--only', 'Answers.JobQueue.fail.promise.1']):
            items = records(run(binary, ['test', '--json', *only, str(answer)], 0), 'test')
            promises = [r for r in items if r.get('kind') == 'promise']
            assert promises and all(r['outcome'] == 'passed' for r in promises), items
            assert items[-1]['errors'] == 0, items
        # Half the random trials take the Queue that the Lease's own lease call returned (D284),
        # so acknowledge and fail reach their Ok branch: breaking either Ok path fails its promise
        # and only that one.
        honest = answer.read_text()
        for failing, honest_line, broken_line in (
                ('acknowledge', 'if completed?: Acknowledged', 'if completed?: Dead'),
                ('fail', 'settled.ready_at = now', 'settled.ready_at = entry.ready_at')):
            assert honest.count(honest_line) == 1, honest_line
            answer.write_text(honest.replace(honest_line, broken_line))
            items = records(run(binary, ['test', '--json', str(answer)], 1), 'test')
            outcomes = {r['name']: r['outcome'] for r in items if r.get('kind') == 'promise'}
            assert outcomes == {f'Answers.JobQueue.{name}.promise.1':
                                'failed' if name == failing else 'passed'
                                for name in ('submit', 'expire_leases', 'lease', 'acknowledge',
                                             'fail')}, items
            assert items[-1]['errors'] == 0, items


def composite_promises(binary):
    with tempfile.TemporaryDirectory(prefix='mo-composite-promises-') as directory:
        path = pathlib.Path(directory) / 'main.mo'
        (path.parent / 'shapes.mo').write_text(
            'module Shapes\nexpose Pair(..)\nstruct Pair(Z, A)\nfirst: Z\nsecond: A\nend\n')
        path.write_text('module Main\nuse Shapes{Pair}\n'
                        'fn probe(value: Pair(List(Int), (Text, Bool))) : Bool\n'
                        'promise "keeps the composite shape"\nresult == value.second.1\n'
                        'allows value: Pair(second: ("owned", true), first: [3, 8]), result: true\n'
                        'forbids value: Pair(first: [], second: ("é", false)), result: true\nend\n'
                        'value.second.1\nend\ntest "after promise"\nassert true\nend\n'
                        'fn main()\nstop "main ran"\nend\n')
        items = records(run(binary, ['test', '--json', str(path)], 0), 'test')
        assert [(r['name'], r['outcome']) for r in items[1:-1]] == [
            ('Main.probe.promise.1', 'passed'), ('Main.test.1', 'passed')]
        assert items[-1]['passed'] == 2 and items[-1]['errors'] == 0
        for codec in ('core', 'bytecode'):
            emitted = run(binary, ['check', '--emit=' + codec, str(path)], 0)
            artifact = path.parent / ('composite.' + codec)
            artifact.write_bytes(emitted.stdout)
            run(binary.parent / 'tests/test_cli.test', ['--roundtrip-' + codec, str(artifact)], 0)

        # Declaration order is not alphabetical or parameter-letter order.
        # Two independent predicates pin both false-line locations and the
        # owned tuple result, not merely that composite generation ran.
        (path.parent / 'shapes.mo').write_text(
            'module Shapes\nexpose Pair(..)\nstruct Pair(Z, A)\nzeta: Z\nalpha: A\nend\n')
        source = ('module Main\nuse Shapes{Pair}\n'
                  'fn probe(value: Pair(Text, Int), _suffix: Text) : (Text, Int)\n'
                  'promise "ordered owned fields"\nresult.1 == value.alpha + 17\n'
                  'result.0 != "é" or _suffix != ""\n'
                  'allows _suffix: "tail", value: Pair(alpha: 3, zeta: "é"), result: ("é", 20)\n'
                  'forbids value: Pair(zeta: "é", alpha: 3), _suffix: "", result: ("é", 20)\nend\n'
                  '(value.zeta, value.alpha + 17)\nend\n'
                  'test "after promise"\nassert true\nend\nfn main()\nend\n')
        path.write_text(source)
        items = records(run(binary, ['test', '--json', str(path)], 1), 'test')
        result = items[1]
        assert result['input'] == [{'name': 'value', 'value': 'Pair(zeta: "é", alpha: 0)'},
                                    {'name': '_suffix', 'value': '""'}]
        assert result['result'] == '("é", 17)' and result['outcome'] == 'failed'
        predicate = 'result.0 != "é" or _suffix != ""'
        start = source.encode().index(predicate.encode())
        assert (result['line']['start'], result['line']['end']) == (start, start + len(predicate.encode()))
        assert items[-1]['failed'] == items[-1]['passed'] == 1 and items[-1]['errors'] == 0
        source = source.replace('result.1 == value.alpha + 17', 'result.1 == value.alpha + 18')
        source = source.replace('result: ("é", 20)\nforbids', 'result: ("é", 21)\nforbids')
        path.write_text(source)
        items = records(run(binary, ['test', '--json', str(path)], 1), 'test')
        assert items[1]['input'] == [{'name': 'value', 'value': 'Pair(zeta: "", alpha: 0)'},
                                    {'name': '_suffix', 'value': '""'}]
        assert items[1]['result'] == '("", 17)'
        predicate = 'result.1 == value.alpha + 18'
        start = source.encode().index(predicate.encode())
        assert (items[1]['line']['start'], items[1]['line']['end']) == (start, start + len(predicate))

        # Lists must actually execute empty, single and several-element
        # boundaries. A pass from an empty-only generator cannot satisfy this.
        for pattern, expected in (('[]', '[]'), ('[_]', '[""]'),
                                   ('[_, _, _]', '["", "a", "é"]')):
            source = ('module Main\nfn probe(value: List(Text)) : Int\n'
                      'promise "list boundaries"\nresult == 7\n'
                      'allows value: ["owned", "é"], result: 7\n'
                      'forbids value: [], result: 8\nend\ncase value\n' +
                      pattern + ': stop "chosen list shape"\n_: 7\nend\nend\nfn main()\nend\n')
            path.write_text(source)
            items = records(run(binary, ['test', '--json', '--calls', '3', str(path)], 1), 'test')
            assert items[1]['outcome'] == 'stopped' and items[1]['input'] == [
                {'name': 'value', 'value': expected}]
            assert items[1]['defect']['detail'] == 'chosen list shape'

        # Public builtin choices cover every case and shrink owned asymmetric
        # payloads; rejecting a payload must never become a None/Ok-only pass.
        cases = [
            ('Option(Text)', 'Some("owned")', 'None', 'result is Some(_)', 'None'),
            ('Option(Text)', 'Some("owned")', 'Some("é")', 'result != Some("é")', 'Some("é")'),
            ('Result((Text, Int), List(Text))', 'Error(["owned"])', 'Ok(("é", 0))',
             'result != Ok(("é", 0))', 'Ok(("é", 0))'),
            ('Result((Text, Int), List(Text))', 'Ok(("owned", 13))', 'Error(["é"])',
             'result != Error(["é"])', 'Error(["é"])'),
        ]
        for type_, allowed, forbidden, predicate, expected in cases:
            source = ('module Main\n' + f'fn probe(value: {type_}) : {type_}\n'
                      f'promise "public choice payloads"\n{predicate}\n'
                      f'allows value: {allowed}, result: {allowed}\n'
                      f'forbids value: {forbidden}, result: {forbidden}\nend\n'
                      'value\nend\nfn main()\nend\n')
            path.write_text(source)
            items = records(run(binary, ['test', '--json', str(path)], 1), 'test')
            assert items[1]['outcome'] == 'failed' and items[1]['input'] == [
                {'name': 'value', 'value': expected}]
            assert items[1]['result'] == expected and items[-1]['errors'] == 0

        # The minimum recursive inhabitant is End even though declared last.
        # Shrinking a depth violation must retain two nodes, not truncate to a
        # passing End or recurse forever through the template's back edge.
        source = ('module Main\nenum Chain(T)\nNode(value: T, next: Chain(T))\nEnd\nend\n'
                  'fn depth(value: Chain(Text)) : Int\ncase value\n'
                  'End: 0\nNode(next: rest): 1 + depth(rest)\nend\nend\n'
                  'fn probe(value: Chain(Text)) : Int\npromise "short chain"\nresult < 2\n'
                  'allows value: Node(value: "owned", next: End), result: 1\n'
                  'forbids value: End, result: 2\nend\ndepth(value)\nend\nfn main()\nend\n')
        path.write_text(source)
        items = records(run(binary, ['test', '--json', str(path)], 1), 'test')
        assert items[1]['outcome'] == 'failed' and items[1]['input'] == [
            {'name': 'value', 'value': 'Node(value: "", next: Node(value: "", next: End))'}]
        assert items[1]['result'] == '2' and items[-1]['errors'] == 0

        # Every declared case, including named multi-field payloads, must be
        # reached. Recursive Lists add a finite non-minimum shell, not just [].
        declarations = 'enum Choice\nQuiet\nNumber(Int)\nOwned(Text)\nPacket(zeta: Text, alpha: Bool)\nend\n'
        cases = [(declarations, 'Choice', 'Quiet', 'Quiet', 'Quiet'),
                 (declarations, 'Choice', 'Quiet', 'Number(_)', 'Number(0: 0)'),
                 (declarations, 'Choice', 'Quiet', 'Owned(_)', 'Owned(0: "")'),
                 (declarations, 'Choice', 'Quiet', 'Packet(zeta: _, alpha: _)',
                  'Packet(zeta: "", alpha: false)'),
                 ('enum Forest\nBranch(List(Forest))\nBud(Text)\nend\n', 'Forest',
                  'Bud("owned")', 'Branch([_])', 'Branch(0: [Branch(0: [])])')]
        for declarations, type_, anchor, pattern, expected in cases:
            source = ('module Main\n' + declarations + f'fn probe(value: {type_}) : Int\n'
                      'promise "all case boundaries"\nresult == 7\n'
                      f'allows value: {anchor}, result: 7\nforbids value: {anchor}, result: 8\nend\n'
                      f'if value is {pattern}\nstop "chosen case"\nend\n7\nend\nfn main()\nend\n')
            path.write_text(source)
            items = records(run(binary, ['test', '--json', str(path)], 1), 'test')
            assert items[1]['outcome'] == 'stopped' and items[1]['input'] == [
                {'name': 'value', 'value': expected}]
            assert items[1]['defect']['detail'] == 'chosen case' and items[-1]['errors'] == 0

        # The source inventory belongs to the declaring module, even with
        # concrete nominal inputs reached through an importing entry module.
        (path.parent / 'shapes.mo').write_text(
            'module Shapes\nexpose Pair(..), probe\nstruct Pair(Z, A)\nzeta: Z\nalpha: A\nend\n'
            'fn probe(value: Pair(Text, Int)) : Int\npromise "declaring module"\nresult == value.alpha\n'
            'allows value: Pair(zeta: "owned", alpha: 13), result: 13\n'
            'forbids value: Pair(zeta: "é", alpha: 29), result: 31\nend\nvalue.alpha + 1\nend\n')
        path.write_text('module Main\nuse Shapes\nfn main() : Int\n'
                        'Shapes.probe(Shapes.Pair(zeta: "main", alpha: 33))\nend\n')
        items = records(run(binary, ['test', '--json', '--list', '--max-heap', '1', str(path)], 0), 'test')
        assert [r['name'] for r in items[1:-1]] == ['Shapes.probe.promise.1']
        span = items[1]['span']
        assert span['file'] == str(path.parent / 'shapes.mo')
        items = records(run(binary, ['test', '--json', '--in', 'Shapes', str(path)], 1), 'test')
        assert items[1]['span'] == span and items[1]['result'] == '1'
        assert items[1]['input'] == [{'name': 'value', 'value': 'Pair(zeta: "", alpha: 0)'}]
        assert items[1]['line']['file'] == span['file'] and items[-1]['errors'] == 0


def composite_deadlines(binary):
    clock = binary.parent / 'tests/test_deadline.test'
    loop = 'var spin = 0\nwhile spin == 0\nspin = 0\nend\n'
    tail = 'test "after promise"\nassert true\nend\nfn main()\nstop "main ran"\nend\n'
    with tempfile.TemporaryDirectory(prefix='mo-composite-deadlines-') as directory:
        path = pathlib.Path(directory) / 'main.mo'
        for where in ('subject', 'condition', 'stop'):
            operation = 'stop "composite candidate stopped"\n' if where == 'stop' else loop
            source = ('module Main\nstruct Packet\nzeta: Text\nalpha: (Text, Int)\nend\n'
                      'struct Box\nvalue: (Text, (Text, Int))\nmark: Int\nend\n'
                      'fn to_text(_value: Box) : Text\nstop "renderer called customizer"\nend\n'
                      'fn verdict(value: Packet, answer: Box) : Bool\n' +
                      ('if value.zeta.byte_count > 2 and value.zeta.byte_count < 1024\n' + operation +
                       'end\n' if where != 'subject' else '') +
                      'answer.value.0 == value.zeta and answer.value.0.byte_count < 1024\nend\n'
                      'fn probe(value: Packet) : Box\npromise "retained composite failure"\n'
                      'result.value == (value.zeta, value.alpha)\nverdict(value, result)\n'
                      'allows value: Packet(alpha: ("owned", 13), zeta: "ok"), '
                      'result: Box(mark: 17, value: ("ok", ("owned", 13)))\n'
                      'forbids value: Packet(zeta: "ok", alpha: ("owned", 13)), '
                      'result: Box(value: ("bad", ("tail", 29)), mark: 31)\nend\n' +
                      ('if value.zeta.byte_count > 2 and value.zeta.byte_count < 1024\n' + operation +
                       'end\n' if where == 'subject' else '') +
                      'Box(value: (value.zeta, value.alpha), mark: 17)\nend\n')
            path.write_text(source + tail)
            for mode in (('fixed',) if where == 'stop' else ('expiry', 'failure')):
                items = records(run(clock, [mode, 'test', '--json', '--seconds',
                                             '2' if mode == 'failure' else '1', str(path)], 1), 'test')
                result = items[1]
                length = 512 if where == 'stop' else 1024
                text = '"' + 'x' * length + '"'
                packet = f'Packet(zeta: {text}, alpha: ("", 0))'
                box = f'Box(value: ({text}, ("", 0)), mark: 17)'
                assert result['input'] == [{'name': 'value', 'value': packet}]
                assert items[2]['name'] == 'Main.test.1' and items[2]['outcome'] == 'passed'
                if where == 'stop':
                    assert result['outcome'] == 'stopped' and 'result' not in result and 'line' not in result
                    defect = result['defect']
                    assert defect['detail'] == 'composite candidate stopped'
                    assert defect['variables'] == [{'name': 'value', 'value': packet},
                                                    {'name': 'answer', 'value': box}]
                    assert [frame['function'] for frame in defect['stack']] == [
                        'Main.verdict', 'Main.probe.promise.1']
                    start = source.encode().index(b'stop "composite candidate stopped"')
                    assert (defect['span']['start'], defect['span']['end']) == (start, start + 34)
                    assert items[-1]['stopped'] == 1 and items[-1]['failed'] == 0
                else:
                    assert result['outcome'] == 'failed' and result['result'] == box
                    start = source.encode().index(b'verdict(value, result)')
                    assert (result['line']['start'], result['line']['end']) == (start, start + 22)
                    assert items[-1]['failed'] == 1 and items[-1]['stopped'] == 0
                assert [r['code'] for r in items if r['record'] == 'diagnostic'] == (
                    ['E1404'] if mode == 'failure' else [])

        path.write_text('module Main\nfn probe(_value: List(Text)) : Int\n'
                        'promise "unresolved composite"\nresult == 7\n'
                        'allows _value: ["owned"], result: 7\nforbids _value: [], result: 8\nend\n' +
                        loop + '7\nend\n' + tail)
        items = records(run(clock, ['expiry', 'test', '--json', '--seconds', '1', str(path)], 1), 'test')
        assert [r['name'] for r in items if r['record'] == 'result'] == ['Main.test.1']
        assert items[-1] == {'record': 'totals', 'errors': 1, 'dropped': 0, 'passed': 1,
                             'failed': 0, 'stopped': 0, 'status': 1}
        assert items[-2]['code'] == 'E1404'


def collection_promises(binary):
    # Map and Set inputs are generated like List: empty first, then entries,
    # and a failing input shrinks to the smallest map or set that still fails.
    with tempfile.TemporaryDirectory(prefix='mo-collection-promises-') as directory:
        path = pathlib.Path(directory) / 'main.mo'
        body = ('promise "{sentence}"\nresult == 0\n'
                'allows value: {{}}, tags: Set.empty, result: 0\n'
                'forbids value: {{"a": 1}}, tags: Set.empty, result: 1\nend\n')
        for subject, sentence, expected in (
                ('value.entries.size * 0 + tags.size * 0', 'a map and a set give zero', None),
                ('value.entries.size + tags.size', 'a map and a set are empty',
                 [{'name': 'value', 'value': '{"": 0}'}, {'name': 'tags', 'value': '[].to_set'}])):
            path.write_text('module Main\nfn probe(value: Map(Text, Int), tags: Set(Int)) : Int\n' +
                            body.format(sentence=sentence) + subject + '\nend\n'
                            'test "after promise"\nassert true\nend\nfn main()\nend\n')
            items = records(run(binary, ['test', '--json', str(path)], 0 if expected is None else 1),
                            'test')
            if expected is None:
                assert [(r['name'], r['outcome']) for r in items[1:-1]] == [
                    ('Main.probe.promise.1', 'passed'), ('Main.test.1', 'passed')]
                assert items[-1]['passed'] == 2 and items[-1]['errors'] == 0
            else:
                assert items[1]['outcome'] == 'failed' and items[1]['input'] == expected
                assert items[1]['result'] == '1' and items[-1]['errors'] == 0

        # Maps and Sets nest under records and Options like any other input.
        path.write_text('module Main\nstruct Bag\nitems: Map(Int, Set(Text))\nend\n'
                        'fn probe(value: Option(Bag)) : Int\npromise "nested collections"\n'
                        'result == 0\nallows value: None, result: 0\n'
                        'forbids value: None, result: 1\nend\n'
                        'case value\nNone: 0\nSome(bag): bag.items.entries.size\nend\nend\n'
                        'fn main()\nend\n')
        items = records(run(binary, ['test', '--json', str(path)], 1), 'test')
        assert items[1]['outcome'] == 'failed' and items[1]['input'] == [
            {'name': 'value', 'value': 'Some(Bag(items: {0: [].to_set}))'}]

        # A set of text keeps its own elements, so a rejected element shrinks to
        # the one smallest member rather than to the passing empty set.
        path.write_text('module Main\nfn probe(tags: Set(Text)) : Int\n'
                        'promise "a set is empty"\nresult == 0\n'
                        'allows tags: Set.empty, result: 0\n'
                        'forbids tags: Set.empty, result: 1\nend\ntags.size\nend\nfn main()\nend\n')
        items = records(run(binary, ['test', '--json', str(path)], 1), 'test')
        assert items[1]['outcome'] == 'failed' and items[1]['input'] == [
            {'name': 'tags', 'value': '[""].to_set'}]


def promise_selection(binary):
    opaque = 'expose Box\nstruct Box\nvalue: Int\nend\n'
    cases = [
        ('fn(Int) : Int', 'identity', 'fn identity(value: Int) : Int\nvalue\nend\n'),
        ('List(fn(Int) : Int)', '[identity]', 'fn identity(value: Int) : Int\nvalue\nend\n'),
        ('Box', 'Box(value: 3)', opaque),
        ('List(Box)', '[]', opaque),
        ('Option(Box)', 'None', opaque),
        ('Result(Int, Box)', 'Ok(13)', opaque),
        ('Choice', 'Open', opaque + 'enum Choice\nOpen\nClosed(Box)\nend\n'),
        ('(Text, Box)', '("owned", Box(value: 3))', opaque),
    ]
    # Shapes can start a Box with its exposed make, so these are built through it (reference 12.2).
    buildable = [
        ('Box', 'make(3)', 'use Shapes{Box, make}\n'),
        ('List(Shapes.Box)', '[]', 'use Shapes\n'),
        ('Shapes.Box', 'Shapes.make(3)',
         'expose Box(..)\nuse Shapes\nstruct Box\nother: Text\nend\n'),
    ]
    with tempfile.TemporaryDirectory(prefix='mo-promise-selection-') as directory:
        path = pathlib.Path(directory) / 'main.mo'
        (path.parent / 'shapes.mo').write_text(
            'module Shapes\nexpose Box, make\nstruct Box\nvalue: Int\nend\n'
            'fn make(value: Int) : Box\nBox(value: value)\nend\n')
        for type_, input_, declarations in cases + buildable:
            built = (type_, input_, declarations) in buildable
            initializer = 'seed = crash()\nfn crash() : Int\nstop "must not initialize"\nend\n'
            source = ('module Main\n' + declarations + initializer +
                            'test "first observation"\nassert true\nend\n'
                            'fn supported(value: List(Text)) : List(Text)\n'
                            'promise "supported first"\nresult == value\n'
                            'allows value: ["owned"], result: ["owned"]\n'
                            'forbids value: [], result: ["é"]\nend\nvalue\nend\n'
                            f'fn probe(_value: {type_}) : Int\npromise "seven"\nresult == 7\n'
                            f'allows _value: {input_}, result: 7\nforbids _value: {input_}, result: 8\nend\n'
                            '7\nend\nfn main()\nstop "main ran"\nend\n')
            path.write_text(source)
            items = records(run(binary, ['test', '--json', '--list', '--max-heap', '1', str(path)], 0), 'test')
            assert [r['name'] for r in items[1:-1]] == [
                'Main.test.1', 'Main.supported.promise.1', 'Main.probe.promise.1']
            items = records(run(binary, ['test', '--json', str(path)], 1), 'test')
            assert len(items) == 3 and items[1]['code'] == ('E1406' if built else 'E1400')
            if not built:
                assert items[1]['primary']['label'] == 'Main.probe.promise.1'
            assert items[-1]['passed'] == items[-1]['failed'] == items[-1]['stopped'] == 0
            # Selection, not the mere presence of a deferred promise, controls
            # preflight. Initialization is reached for ordinary-only selection.
            items = records(run(binary, ['test', '--json', '--only', 'Main.test.1', str(path)], 1), 'test')
            assert items[1]['code'] == 'E1406' and items[-1]['passed'] == 0
            path.write_text(source.replace(initializer, ''))
            items = records(run(binary, ['test', '--json', '--only', 'Main.test.1', str(path)], 0), 'test')
            assert items[1]['outcome'] == 'passed' and items[-1]['passed'] == 1
            if built:
                items = records(run(binary, ['test', '--json', str(path)], 0), 'test')
                assert [r['outcome'] for r in items[1:-1]] == ['passed'] * 3

        # Same-module written access to exported opaque fields remains legal;
        # only structural generation is refused. A private local type is not
        # accidentally classified as exported opaque.
        source = ('module Main\nexpose Box\nstruct Box\nvalue: Int\nend\n'
                  'fn probe(value: Box) : Int\npromise "visible only to source"\nresult == value.value\n'
                  'allows value: Box(value: 13), result: 13\n'
                  'forbids value: Box(value: 13), result: 14\nend\nvalue.value\nend\n'
                  'test "own fields"\nassert Box(value: 13).value == 13\nend\nfn main()\nend\n')
        path.write_text(source)
        records(run(binary, ['check', '--json', str(path)], 0), 'check')
        items = records(run(binary, ['test', '--json', '--only', 'Main.test.1', str(path)], 0), 'test')
        assert items[-1]['passed'] == 1
        items = records(run(binary, ['test', '--json', str(path)], 1), 'test')
        assert items[1]['code'] == 'E1400' and items[-1]['passed'] == 0
        path.write_text(source.replace('expose Box\n', ''))
        items = records(run(binary, ['test', '--json', str(path)], 0), 'test')
        assert items[-1]['passed'] == 2 and items[-1]['errors'] == 0


def fix_command(binary):
    """`mo fix` applies every fix offered alone, checks again, and reports what is left (MO-48)."""
    with tempfile.TemporaryDirectory(prefix='mo-fix-') as directory:
        folder = pathlib.Path(directory)
        # The acceptance: after mo fix, mo check exits 0.
        alone = folder / 'alone'
        alone.mkdir()
        fixable = alone / 'main.mo'
        fixable.write_bytes((root / 'tests/cli/fix/main.mo').read_bytes())
        fixable.chmod(0o640)
        result = run(binary, ['fix', str(fixable)], 0)
        assert not result.stdout and not result.stderr, result
        assert fixable.read_text() == 'module Main\nfn run(_value: Int) : Int\n  1\nend\n'
        assert fixable.stat().st_mode & 0o777 == 0o640
        run(binary, ['check', str(fixable)], 0)
        result = run(binary, ['fix', str(fixable)], 0)
        assert not result.stdout and not result.stderr, result
        assert not list(alone.glob('.mo-write-*'))

        # The removed repeated import uncovers the unnamed field, fixed on the next round. Only the
        # given file is rewritten; the imported module's fix is reported, and so is the unused
        # parameter, which has no fix while the module imports names.
        main = folder / 'main.mo'
        shapes = folder / 'shapes.mo'
        main.write_text('module Main\n\nuse Shapes { Shape, Shape }\n\n'
                        'fn area(shape: Shape, scale: Int) : Int\n  case shape\n'
                        '    Square(s): s * s\n  end\nend\n\nfn main()\nend\n')
        shapes_source = ('module Shapes\n\nexpose Shape(..)\n\nenum Shape\n  Square(side: Int)\n'
                         'end\n\nfn unused(count: Int) : Int\n  1\nend\n')
        shapes.write_text(shapes_source)
        result = run(binary, ['fix', str(main)], 1)
        assert not result.stdout
        assert result.stderr.count(b'error ') == 2, result.stderr
        assert b'main.mo:5:23' in result.stderr and b'shapes.mo:9:11' in result.stderr
        assert main.read_text() == ('module Main\n\nuse Shapes { Shape }\n\n'
                                    'fn area(shape: Shape, scale: Int) : Int\n  case shape\n'
                                    '    Square(side: s): s * s\n  end\nend\n\nfn main()\nend\n')
        assert shapes.read_text() == shapes_source
        # Given both files, each is checked as the program it starts and fixed. An error in a
        # given module is reported once, by its own file's check, not by every program using it.
        shapes.write_text(shapes_source + '\nfn bad() : Int\n  "x"\nend\n')
        result = run(binary, ['fix', str(main), str(shapes)], 1)
        assert result.stderr.count(b'error ') == 2, result.stderr
        assert b'main.mo:5:23' in result.stderr and result.stderr.count(b'shapes.mo:14:3') == 1
        assert 'fn unused(_count: Int)' in shapes.read_text()
        result = run(binary, ['fix', str(main)], 1)
        assert b'shapes.mo:14:3' in result.stderr

        # A fixed file that would outgrow the file size limit is left as it was.
        full = folder / 'full' / 'main.mo'
        full.parent.mkdir()
        head = b'module Main\nfn run(value: Int) : Int\n  1\nend\n'
        line = b'# ' + b'x' * 77 + b'\n'
        rest = 8 * 1024 * 1024 - len(head)
        body = line * (rest // len(line))
        full_source = head + body + b'#' * (rest - len(body) - 1) + b'\n'
        assert len(full_source) == 8 * 1024 * 1024
        full.write_bytes(full_source)
        result = run(binary, ['fix', str(full)], 1)
        assert b'E1407: the fixed file exceeds' in result.stderr and b'E0052' in result.stderr
        assert full.read_bytes() == full_source

        # A file fix can't replace is refused like format's, and the rest are still fixed.
        fixable.write_bytes((root / 'tests/cli/fix/main.mo').read_bytes())
        link = folder / 'link.mo'
        link.symlink_to(fixable)
        result = run(binary, ['fix', str(link), str(folder / 'missing.mo'), str(fixable)], 1)
        assert result.stderr.count(b'E1407') == 2 and result.stderr.count(b'error ') == 2
        assert link.is_symlink() and '_value' in fixable.read_text()
        assert not list(folder.glob('.mo-write-*'))


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
            assert items[1]["code"] == "E1103" and items[-1]["errors"] == 1

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
            ("JobQueue.test.1", "passed")]
        assert job_queue_tests[-1] == {"record": "totals", "errors": 0, "dropped": 0,
                                       "passed": 9, "failed": 0, "stopped": 0, "status": 0}
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


def guide_claims(binary):
    """`mo guide` tells agents what this build does; each claim is checked against the build.

    When one of these fails, the build changed: update docs/agent-guide.md to match, then this
    test. Never edit only the test."""
    guide = (root / "docs/agent-guide.md").read_text(encoding="utf-8")
    blocks = guide.split("```mo\n")
    assert len(blocks) == 2, "the guide has exactly one Mo program"
    program = blocks[1].split("```\n", 1)[0]

    def says(*sentences):
        for sentence in sentences:
            assert sentence in guide, f"docs/agent-guide.md no longer says: {sentence}"

    def refuses(args, code, headline, claim):
        # The claim is the guide's own sentence, so it must name this code and this headline.
        assert f"E{code}" in claim and f'"{headline}"' in claim, claim
        says(claim)
        result = run(binary, args, 1)
        assert not result.stdout, (args, result.stdout)
        assert f"error E{code}: {headline}".encode() in result.stderr, (args, result.stderr)

    says("This build runs `mo check`, `mo run`, `mo test`, `mo format`, `mo fix`, `mo guide` and `mo emit`.",
         "The program below checks with no errors, runs, and passes its examples and its test.",
         "E1103 and E1150 mean this toolchain has not implemented that form.",
         "`mo run` and `mo test` report the same code for the same form.",
         "accept a remaining diagnostic only when Know this build names it: E1103 or E1150 on a "
         "form the reference requires and offers no other way to write the same behaviour, or "
         "E1400 on a promise whose inputs `mo test` cannot build.",
         "If the reference offers another way to write the same behaviour from forms that "
         "compile, use that instead.",
         "Deleting a required spec, promise or capability to silence a diagnostic leaves the "
         "task undone.")
    with tempfile.TemporaryDirectory(prefix="mo-guide-") as directory:
        folder = pathlib.Path(directory)
        greet = folder / "greet.mo"
        greet.write_text(program, encoding="utf-8")
        result = run(binary, ["check", str(greet)], 0)
        assert not result.stdout and not result.stderr, result
        result = run(binary, ["test", str(greet)], 0)
        assert not result.stderr and result.stdout.endswith(b"3 passed, 0 failed, 0 stopped\n")
        assert run(binary, ["run", str(greet), "Ada", "Grace"], 0).stdout == b"Hello, Ada!\n"
        assert run(binary, ["run", str(greet)], 0).stdout == b"Hello, world!\n"
        result = run(binary, ["format", str(greet)], 0)
        assert not result.stdout and not result.stderr
        assert greet.read_text(encoding="utf-8") == program, "the guide's program is formatted"

        result = run(binary, ["fix", str(greet)], 0)
        assert not result.stdout and not result.stderr
        assert greet.read_text(encoding="utf-8") == program
        says("`mo fix FILE` makes each edit that `mo check` shows as an error's only fix, checks "
             "again, and prints the errors that are left, as `mo check` does. It rewrites only "
             "the files it is given.",
             "`mo format` and `mo fix` rewrite the files they are given.")
        says("`mo emit FILE -o OUT` checks the program as `mo check` does, then writes its bytecode "
             "to `OUT`. `-o` is required, and `OUT` is never a source file of the program. A library module "
             "with no `main` is E0079, as for `mo run`, and a file that cannot be written is "
             "E1404. This build has no command that runs a bytecode file; run the source with "
             "`mo run`.")
        unused = folder / "unused.mo"
        unused.write_text("module Unused\n\nfn one(x: Int) : Int\n  1\nend\n", encoding="utf-8")
        assert b"Fix: " in run(binary, ["check", str(unused)], 1).stderr
        result = run(binary, ["fix", str(unused)], 0)
        assert not result.stdout and not result.stderr
        assert "fn one(_x: Int)" in unused.read_text(encoding="utf-8")

        # Each unfinished form gives the same code from check, run and test.
        unfinished = folder / "unfinished.mo"
        unfinished.write_text("module Unfinished\n\nfn main(console: Console)\n"
                              "  _printed = console.printed\nend\n", encoding="utf-8")
        deferred = folder / "deferred.mo"
        deferred.write_text('module Deferred\nhandler = fn() base end\nbase = 13\n'
                            'fn main() : Int\nhandler()\nend\n'
                            'test "reads base"\nassert handler() == 13\nend\n', encoding="utf-8")
        for command in ("check", "run", "test"):
            refuses([command, str(unfinished)], 1103,
                    "this body rule is not implemented by the staged checker",
                    "`mo check` reports E1103 for a form the checker has not implemented, with a "
                    'headline that says so, such as "this body rule is not implemented by the '
                    'staged checker".')
            refuses([command, str(deferred)], 1150,
                    "this constant's deferred function needs globals not yet initialized",
                    "It reports E1150 for a form it checks but cannot compile yet, such as a "
                    "constant's function value that reads a later constant. The headline is "
                    '"this constant\'s deferred function needs globals not yet initialized".')

        # E1400 for a promise whose function takes a function; the rest run with --only.
        twice = folder / "twice.mo"
        twice.write_text('module Twice\n\nfn twice(xs: List(Int), f: fn(Int) : Int) : List(Int)\n'
                         '  promise "keeps the length"\n    result.size == xs.size\n'
                         '    allows xs: [1], f: fn(x) x end, result: [1]\n'
                         '    forbids xs: [1], f: fn(x) x end, result: []\n  end\n\n'
                         '  xs.map(fn(x) f(f(x)) end)\nend\n\n'
                         'fn one() : Int\n  examples\n    one() == 1\n  end\n\n  1\nend\n',
                         encoding="utf-8")
        run(binary, ["check", str(twice)], 0)
        refuses(["test", str(twice)], 1400, "this build cannot generate inputs for this promise",
                "`mo test` exits 1 with E1400 when it cannot build the inputs of a selected "
                "promise, such as one whose function takes a function. The headline is "
                '"this build cannot generate inputs for this promise".')
        says("Run the other examples, promises and tests by name with `--only`, as `--list` "
             "prints them.")
        listed = run(binary, ["test", "--list", str(twice)], 0).stdout
        assert listed == b"Twice.twice.promise.1\nTwice.one.example.1\n", listed
        result = run(binary, ["test", "--only", "Twice.one.example.1", str(twice)], 0)
        assert result.stdout.endswith(b"1 passed, 0 failed, 0 stopped\n")

    says("The `lib/` folder holds library modules written in Mo, and every `mo` command loads "
         "them with your program.",
         "`mo check lib/list.mo` and `mo test lib/list.mo` check and test one of them on its own.")
    for library_file in sorted((root / "lib").glob("*.mo")):
        run(binary, ["check", str(library_file)], 0)
        result = run(binary, ["test", str(library_file)], 0)
        assert result.stdout.endswith(b", 0 failed, 0 stopped\n"), (library_file, result.stdout)
    with tempfile.TemporaryDirectory() as folder:
        source = pathlib.Path(folder) / "main.mo"
        source.write_text("module Main\nuse Json\nfn main(console: Console)\n"
                          "  console.print(Json.Null.to_text)\nend\n"
                          "test \"own\"\n  assert 1 == 1\nend\n")
        core = run(binary, ["check", "--emit=core", str(source)], 0).stdout
        assert b'"Json.' in core and b"Main.test.1" in core, core[:200]
        for kind in (b".example.", b".promise.", b".allows.", b".forbids."):
            assert kind not in core, ("a program lowers a library module's " + kind.decode(), core[:200])
        assert core.count(b".test.") == core.count(b"Main.test.1"), core[:200]
    for name in ("map", "filter", "filter_map", "flat_map", "reduce", "find", "any?", "all?", "count",
                 "sort_by", "sort_by_desc", "sort", "min", "max", "group_by", "Map.update",
                 "Option.or_fail", "Option.map", "Result.map", "Result.map_error",
                 "FileError.to_text", "Json.parse", "pretty"):
        says(f"`{name}`")
    says("`lib/text.mo` has no functions, only tests of `Text`'s operations")
    says("`lib/set.mo` likewise tests `Set`'s operations, `List.to_set` and Set text forms")
    says("`lib/unicode16.mo` is the `Unicode16` module.",
         "After `use Unicode16`, its built-in `graphemes`, `display_width`, `normalize_nfc`, "
         "`lowercase` and `uppercase` operations follow Unicode 16.0.",
         "`mo test lib/unicode16.mo` tests them.")
    says("`Unicode16.letter?(c: Char) : Bool` tests Unicode 16.0 General_Category L",
         "`Unicode16.digit?(c: Char) : Bool` tests `Nd` (decimal digits).",
         "Neither includes `Nl`, `No` or marks, normalizes, or uses locale tailoring.")
    says("After `use Json` (or `use Shop.Account`), a bare `Json` (or `Account`) in a type is the "
         "module's own type of that name, as reference 10.3 says: write `fn f(value: Json)`. "
         "`Json.Json` means the same type.")
    with tempfile.TemporaryDirectory() as folder:
        source = pathlib.Path(folder) / "main.mo"
        program = ("module Main\nuse {use}\nfn f(value: {type}) : Text\n  value.to_text\nend\n"
                   "fn main(console: Console)\n  console.print(f(Json.Null))\nend\n")
        for use, type_name in (("Json", "Json"), ("Json", "Json.Json"), ("Json{Json}", "Json")):
            source.write_text(program.format(use=use, type=type_name))
            result = run(binary, ["run", str(source)], 0)
            assert result.stdout == b"null\n", (use, result.stdout, result.stderr)


def emit_command(binary):
    """mo emit FILE -o OUT writes the verified bytecode that mo run executes (MO-41)."""
    driver = binary.parent / "tests/test_cli.test"
    sources = {
        # Console output and an exit status, so both sides of the comparison have something to match.
        "console": 'module Main\nfn main(console: Console) : Int\nconsole.print("hello 日本語")\n37\nend\n',
        "silent": "module Main\nfn main()\nend\n",
    }
    with tempfile.TemporaryDirectory(prefix="mo-emit-") as directory:
        folder = pathlib.Path(directory)
        for name, text in sources.items():
            path = folder / name / "main.mo"
            path.parent.mkdir()
            path.write_text(text)
            out = folder / f"{name}.bc"
            for args in (["emit", str(path), "-o", str(out)], ["emit", "-o", str(out), str(path)]):
                out.unlink(missing_ok=True)
                result = run(binary, args, 0)
                assert not result.stdout and not result.stderr
                data = out.read_bytes()
                assert data.startswith(b"MOBC")
            # The loader verifies the file as it loads it, then the program prints and exits as
            # mo run does on the source.
            ran = run(binary, ["run", str(path)], 37 if name == "console" else 0)
            loaded = run(driver, ["--run-bytecode", str(out)], 37 if name == "console" else 0)
            assert loaded.stdout == ran.stdout and loaded.stderr == ran.stderr, (name, loaded, ran)
            # Emission is deterministic.
            again = folder / f"{name}-again.bc"
            run(binary, ["emit", str(path), "-o", str(again)], 0)
            assert again.read_bytes() == data
        # Emitting replaces an existing file whole.
        out = folder / "console.bc"
        out.write_bytes(b"x" * (1 << 20))
        run(binary, ["emit", str(folder / "console" / "main.mo"), "-o", str(out)], 0)
        assert out.read_bytes().startswith(b"MOBC") and out.stat().st_size < 1 << 20
        # Checks come first: a program with errors writes nothing and reports them as mo check does.
        bad = folder / "bad" / "main.mo"
        bad.parent.mkdir()
        bad.write_text("module Main\nfn main() : Int\n\"text\"\nend\n")
        target = folder / "bad.bc"
        refused = run(binary, ["emit", str(bad), "-o", str(target)], 1)
        assert refused.stderr == run(binary, ["check", str(bad)], 1).stderr and not target.exists()
        # A library has no entry, as for mo run.
        library = root / "tests/cli/library/main.mo"
        target = folder / "library.bc"
        refused = run(binary, ["emit", str(library), "-o", str(target)], 1)
        assert b"E0079" in refused.stderr and not target.exists()
        # The source is never the output.
        source = folder / "console" / "main.mo"
        before = source.read_bytes()
        refused = run(binary, ["emit", str(source), "-o", str(source)], 1)
        assert b"E1404" in refused.stderr and source.read_bytes() == before
        # Nor is any module the program imports.
        imported = folder / "imported"
        imported.mkdir()
        (imported / "main.mo").write_text(
            "module Main\nuse Left\n\nfn main(console: Console)\nconsole.print(\"#{Left.value()}\")\nend\n")
        left = imported / "left.mo"
        left.write_text("module Left\nexpose value\n\nfn value() : Int\n11\nend\n")
        before = left.read_bytes()
        refused = run(binary, ["emit", str(imported / "main.mo"), "-o", str(left)], 1)
        assert b"E1404" in refused.stderr and left.read_bytes() == before
        # With anchors, emit stops after checking them: it runs neither main nor a failing
        # anchor's program through to a file.
        anchored = folder / "anchored"
        anchored.mkdir()
        anchor_source = ('module Main\nfn twice(n: Int) : Int\npromise "returns twice its input"\n'
                         'result == n + n\nallows result: 6, n: 3\nforbids n: 4, result: 7\nend\n'
                         'n + n\nend\nfn main(console: Console) : Int\nconsole.print("main")\n37\nend\n')
        (anchored / "main.mo").write_text(anchor_source)
        target = folder / "anchored.bc"
        result = run(binary, ["emit", str(anchored / "main.mo"), "-o", str(target)], 0)
        assert not result.stdout and not result.stderr and target.read_bytes().startswith(b"MOBC")
        target.unlink()
        (anchored / "main.mo").write_text(anchor_source.replace("result: 6", "result: 5"))
        refused = run(binary, ["emit", str(anchored / "main.mo"), "-o", str(target)], 1)
        assert b"E1405" in refused.stderr and not refused.stdout and not target.exists()
        # A file that can't be written is E1404 with the OS's reason, and nothing is left behind.
        refused = run(binary, ["emit", str(source), "-o", str(folder / "missing" / "x.bc")], 1)
        assert b"E1404" in refused.stderr and b"No such file or directory" in refused.stderr
        full = pathlib.Path("/dev/full")
        if full.exists():
            refused = run(binary, ["emit", str(source), "-o", str(full)], 1)
            assert b"E1404" in refused.stderr
    def mistake(args, message):
        r = run(binary, args, 2)
        assert not r.stdout and r.stderr == message.encode() + b"\n", (args, r.stderr)

    good = "tests/cli/good/main.mo"
    mistake(["emit", good], 'emit needs -o FILE: "emit"')
    mistake(["emit", "-o", "x.bc"], 'command needs a file: "emit"')
    mistake(["emit", good, "-o"], 'flag needs a nonempty value: "-o"')
    mistake(["emit", good, "-o", ""], 'flag needs a nonempty value: "-o" ""')
    mistake(["emit", good, "-o", "-x.bc"], 'flag needs a nonempty value: "-o" "-x.bc"')
    mistake(["emit", good, "-o", "a.bc", "-o", "b.bc"], 'flag may be given only once (except --only): "-o"')
    mistake(["emit", good, good, "-o", "a.bc"], f'command takes exactly one file: "{good}"')
    for flag in ("--json", "--emit=core", "--max-heap", "--list"):
        args = ["emit", flag, *(["1GiB"] if flag == "--max-heap" else []), good, "-o", "a.bc"]
        mistake(args, f'flag is not accepted by this command: "{flag}"')
    for command in ("check", "run", "test", "format", "fix"):
        mistake([command, "-o", "a.bc", good], f'flag is not accepted by this command: "-o"')
    assert not (root / "a.bc").exists()


def check(binary):
    driver = binary.parent / "tests/test_cli.test"
    faults = binary.parent / "tests/test_cli_faults.test"
    guide_claims(binary)
    emit_command(binary)
    runner(binary)
    lower_source_fixtures(binary)
    anchors(binary)
    constants(binary)
    filenames(binary)
    observations(binary)
    captures(binary)
    imports(binary)
    generic_imports(binary)
    scalar_promises(binary)
    promise_boundaries(binary)
    promise_deadlines(binary)
    promise_shrinking(binary)
    generic_promises(binary)
    fake_capability_promises(binary)
    hidden_promises(binary)
    composite_promises(binary)
    collection_promises(binary)
    composite_deadlines(binary)
    promise_selection(binary)
    imported_constants(binary)
    fix_command(binary)
    items = records(run(faults, ["--json"], 1), "check")
    assert items[1]["code"] == "E1401" and items[-1]["dropped"] == 0
    assert items[1]["primary"]["file"] is None and items[1]["primary"]["file_bytes"] is None
    assert items[1]["section"] is None
    failure = run(faults, ["--text"], 1)
    assert not failure.stdout and b"E1401" in failure.stderr
    for stage in ("parse", "load", "modules", "signatures", "bodies", "program", "fix",
                  "multiline"):
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
    library = "tests/cli/library/main.mo"
    result = run(binary, ["check", library], 0)
    assert not result.stdout and not result.stderr
    run(binary, ["check", "--emit=bytecode", library], 0)
    records(run(binary, ["check", "--json", library], 0), "check")
    assert b"E0079" in run(binary, ["run", library], 1).stderr
    tested_lib = records(run(binary, ["test", "--json", library], 0), "test")
    assert [(r["name"], r["outcome"]) for r in tested_lib[1:-1]] == [("Main.test.1", "passed")]
    test_fixture = "tests/cli/test/main.mo"
    listed_fixture = run(binary, ["test", "--json", "--list", test_fixture], 0)
    assert listed_fixture.stdout == (root / "tests/cli/test/list.golden").read_bytes()
    records(listed_fixture, "test")
    ran_fixture = run(binary, ["test", "--json", test_fixture], 1)
    ran_items = records(ran_fixture, "test")
    for r in ran_items[1:-1]:
        assert type(r["microseconds"]) is int and r["microseconds"] >= 0
        r["microseconds"] = 0
    normalized_json = "".join(json.dumps(r, separators=(",", ":")) + "\n" for r in ran_items).encode()
    assert normalized_json == (root / "tests/cli/test/json.golden").read_bytes()
    for mode in ("text", "json"):
        result = run(driver, [f"--render-{mode}"], 0)
        assert not result.stderr
        assert result.stdout == (root / f"tests/cli/render-{mode}.golden").read_bytes()
        if mode == "json":
            d = json.loads(result.stdout)
            assert d["primary"]["column"] == 2
            assert d["primary"]["file"] == 'badÿ"\\.mo'
            assert d["primary"]["file_bytes"] == b'bad\xff"\\.mo'.hex()
            assert d["secondary"][0]["line"] == 7
            assert d["secondary"][0]["file_bytes"] == b'bad\xff"\\.mo'.hex()
            assert d["secondary"][1]["file"] is None and d["secondary"][1]["file_bytes"] is None
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
    result = run(binary, ["test", "--json", "--list", "--only", "No.match", good], 2)
    assert not result.stdout and result.stderr.count(b"\n") == 1

    def mistake(args, message=None):
        r = run(binary, args, 2)
        assert not r.stdout and r.stderr.count(b"\n") == 1, (args, r)
        if message:
            assert r.stderr == message.encode() + b"\n", (args, r.stderr)

    for args in ([], ["unknown"], [""], ["--version", "extra"], ["check"], ["format"],
                 ["fix"], ["run"], ["test"], ["emit"], ["emit", good], ["guide", good], ["check", ""],
                 ["check", good, good], ["test", good, good], ["check", "--bad", good],
                 ["check", "--emit=bad", good], ["check", "--emit", "core", good],
                 ["check", "--json", "--emit=core", good], ["check", "--version", good],
                 ["check", "--json", "--bad", good]):
        mistake(args)
    mistake(["bad\n\t\xff"], 'unknown command (expected check, run, test, format, fix, guide or emit): "bad\\x0a\\x09\\xc3\\xbf"')
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
    run(binary, ["fix", good, good], 0)
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
    # fix checks each file's program, so it gets fewer copies than format's 4096.
    run(binary, ["format", *([good] * 4096)], 0)
    run(binary, ["fix", *([good] * 64)], 0)
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
        assert records(result, "check")[1]["primary"]["file_bytes"] == os.fsencode(path).hex()
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
        assert d["primary"]["file_bytes"] == os.fsencode(unicode_path).hex()
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
            path = invalid / "bad\udcff.mo"
            path.write_text("module Main\nfn run()\nunknown\nend\n")
            d = records(run(binary, ["check", "--json", str(path)], 1), "check")[1]
            assert "badÿ/badÿ.mo" in d["primary"]["file"]
            assert d["primary"]["file_bytes"] == os.fsencode(path).hex()
            assert d["primary"]["file_bytes"].endswith(b"bad\xff/bad\xff.mo".hex())
            valid_utf8 = folder / "badÿ" / "badÿ.mo"
            valid_utf8.parent.mkdir()
            valid_utf8.write_text("module Main\nfn run()\nunknown\nend\n")
            d_utf8 = records(run(binary, ["check", "--json", str(valid_utf8)], 1), "check")[1]
            assert d_utf8["primary"]["file"] == d["primary"]["file"]
            assert d_utf8["primary"]["file_bytes"] == os.fsencode(valid_utf8).hex()
            assert d_utf8["primary"]["file_bytes"] != d["primary"]["file_bytes"]
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
                # A constant-only library module checks cleanly (D250), while
                # running it still requires main (E0079).
                run(binary, ["check", str(path)], 0)
                result = run(binary, ["run", str(path)], 1)
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
        assert not list(folder.glob('.mo-write-*'))
    print(f"{binary}: CLI goldens, argument matrix and JSON validity passed")


for executable in sys.argv[1:]:
    check(pathlib.Path(executable).resolve())
