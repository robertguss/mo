import pathlib
import tempfile

from cli_common import records, root, run


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
            '_names = files.list(".")\n_line = console.read_line\n_now = clock.now\n'
            '_words = args.list\n0\nend\n'
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

        path.write_text('module Main\nexpose Counter, zero, bump\nstruct Counter\nvalue: Int\nend\n'
                        'zero = Counter(value: 0)\n'
                        'fn bump(counter: Counter) : Counter\nCounter(value: counter.value + 1)\nend\n'
                        'fn count(counter: Counter) : Int\npromise "under two"\nresult < 2\n'
                        'allows counter: zero, result: 0\nforbids counter: zero, result: 2\nend\n'
                        'counter.value\nend\n' + tail)
        items = records(run(binary, ['test', '--json', '--calls', '2', str(path)], 0), 'test')
        assert items[1]['outcome'] == 'passed', items[1]
        items = records(run(binary, ['test', '--json', '--calls', '3', str(path)], 1), 'test')
        assert items[1]['outcome'] == 'failed', items[1]
        assert items[1]['input'] == [{'name': 'counter', 'value': 'Counter(value: 2)'}], items[1]
        assert items[1]['result'] == '2', items[1]

        for typ, measure, anchor, minimum, expected in (
                ('Int', 'value', '2', 2, 'H(value: 7)'),
                ('Text', 'value.byte_count', '"xxx"', 3, 'H(value: "xxxxxxx")')):
            path.write_text('module Main\nexpose H, make\n'
                            f'struct H\nvalue: {typ}\nend\n'
                            f'fn make(value: {typ}) : Option(H)\n'
                            f'if {measure} >= {minimum}: Some(H(value: value)) else: None\nend\n'
                            'fn size(h: H, console: Console) : Int\npromise "under seven"\n'
                            'result < 7 or console.printed.size > 1\n'
                            f'allows h: H(value: {anchor}), console: Console.fake(), result: {minimum}\n'
                            f'forbids h: H(value: {anchor}), console: Console.fake(), result: 7\nend\n'
                            'console.print("once")\n'
                            f'h.{measure}\nend\n' + tail)
            items = records(run(binary, ['test', '--json', str(path)], 1), 'test')
            assert items[1]['outcome'] == 'failed', items[1]
            assert items[1]['input'] == [{'name': 'h', 'value': expected},
                                         {'name': 'console', 'value': '<Console>'}], items[1]
            assert items[1]['result'] == '7', items[1]

        path.write_text('module Main\nexpose H, make\nstruct H\nvalue: Int\nend\n'
                        'fn make(value: Int) : Option(H)\n'
                        'if value >= 2: Some(H(value: value)) else: None\nend\n'
                        'fn probe(a: H, b: H, c: H, d: H) : Int\npromise "zero"\nresult == 0\n'
                        'allows a: H(value: 2), b: H(value: 2), c: H(value: 2), d: H(value: 2), result: 0\n'
                        'forbids a: H(value: 2), b: H(value: 2), c: H(value: 2), d: H(value: 2), result: 1\n'
                        'end\n1 + a.value * 0 + b.value * 0 + c.value * 0 + d.value * 0\nend\n' + tail)
        items = records(run(binary, ['test', '--json', '--calls', '1', str(path)], 1), 'test')
        assert items[1]['calls'] == 201 and items[1]['result'] == '1', items
        assert 'H(value: 2)' in [entry['value'] for entry in items[1]['input']], items
        assert items[-1]['errors'] == 0, items

        path.write_text('module Main\nexpose H, zero, one, ten, hundred\nstruct H\nvalue: Int\nend\n'
                        'zero = H(value: 0)\n'
                        'fn one(h: H) : H\nH(value: h.value + 1)\nend\n'
                        'fn ten(h: H) : H\nH(value: h.value + 10)\nend\n'
                        'fn hundred(h: H) : H\nH(value: h.value + 100)\nend\n'
                        'fn count(h: H) : Int\npromise "not ten or one hundred eleven"\n'
                        'result != 10 and result != 111\n'
                        'allows h: zero, result: 0\nforbids h: zero, result: 10\nend\n'
                        'h.value\nend\n' + tail)
        items = records(run(binary, ['test', '--json', '--calls', '3', str(path)], 1), 'test')
        assert items[1]['input'] == [{'name': 'h', 'value': 'H(value: 10)'}], items
        assert items[1]['result'] == '10' and items[-1]['errors'] == 0, items

        path.write_text('module Main\nexpose H, make\nstruct H\nvalues: List(Int)\nend\n'
                        'fn make(values: List(Int)) : Option(H)\n'
                        'if values.size > 0: Some(H(values: values)) else: None\nend\n'
                        'fn count(h: H) : Int\npromise "under two"\nresult < 2\n'
                        'allows h: H(values: [0]), result: 1\n'
                        'forbids h: H(values: [0]), result: 2\nend\nh.values.size\nend\n' + tail)
        items = records(run(binary, ['test', '--json', str(path)], 1), 'test')
        assert items[1]['input'] == [{'name': 'h', 'value': 'H(values: [0, 0])'}], items
        assert items[1]['result'] == '2' and items[-1]['errors'] == 0, items

        path.write_text('module Main\nexpose H, make\nstruct H\nvalue: Int\n'
                        'payload: List(List(Int))\nend\n'
                        'filler = (0..4).to_list.map(fn(_i) (0..1020).to_list end)\n'
                        'fn make(value: Int) : Option(H)\nif value >= 2\n'
                        'Some(H(value: value, payload: if value > 20: filler else: []))\n'
                        'else\nNone\nend\nend\n'
                        'fn probe(h: H, xs: List(Int)) : Int\npromise "bounded sibling"\n'
                        'result < 7 or xs.size != 5 or not xs.any?(fn(x) x > 1000000 end)\n'
                        'allows h: H(value: 2, payload: []), xs: [0, 0, 0, 0, 1000001], result: 2\n'
                        'forbids h: H(value: 2, payload: []), xs: [0, 0, 0, 0, 1000001], result: 7\n'
                        'end\nh.value + xs.size * 0\nend\n' + tail)
        items = records(run(binary, ['test', '--json', str(path)], 1), 'test')
        assert items[1]['input'] == [{'name': 'h', 'value': 'H(value: 7, payload: [])'},
                                    {'name': 'xs', 'value': '[0, 0, 0, 0, 1000001]'}], items
        assert items[1]['result'] == '7' and items[-1]['errors'] == 0, items

        parameters = ', '.join(f'_a{i}: A' for i in range(29))
        for body, status in (('0', 0), ('1', 1)):
            path.write_text('module Main\nexpose A, B, one, make\nenum A\nOnly\nend\n'
                            'one = Only\nstruct B\nvalue: Int\nend\n'
                            f'fn make({parameters}, flag: Bool) : Option(B)\n'
                            'if flag\n'
                            'Some(B(value: 7))\nelse\nNone\nend\nend\n'
                            'fn probe(a: B, b: B, c: B) : Int\npromise "zero"\nresult == 0\n'
                            'allows a: B(value: 7), b: B(value: 7), c: B(value: 7), result: 0\n'
                            'forbids a: B(value: 7), b: B(value: 7), c: B(value: 7), result: 1\nend\n'
                            f'{body} + a.value * 0 + b.value * 0 + c.value * 0\nend\n' + tail)
            items = records(run(binary, ['test', '--json', '--calls', '1', str(path)], status), 'test')
            failure = next(r for r in items if r.get('kind') == 'promise')
            assert failure['outcome'] == ('failed' if status else 'passed'), items
            assert items[-1]['errors'] == status, items
            if status:
                assert failure['calls'] == 1, items
                assert failure['input'] == [
                    {'name': 'a', 'value': 'B(value: 7)'},
                    {'name': 'b', 'value': 'B(value: 7)'},
                    {'name': 'c', 'value': 'B(value: 7)'}], items
                assert failure['result'] == '1', items
                diagnostic = next(r for r in items if r['record'] == 'diagnostic')
                assert diagnostic['code'] == 'E1404', items
                assert 'builder recording is incomplete' in diagnostic['headline'], items
                again = records(run(binary, ['test', '--json', '--calls', '1', str(path)], 1), 'test')
                repeated = next(r for r in again if r.get('kind') == 'promise')
                for key in ('input', 'result', 'calls', 'line'):
                    assert repeated[key] == failure[key], again

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
        path.write_text(exposed + '\nstruct Pool\nsize: Int\nend\n'
                        'struct Ticket\nseat: Int\nend\nstruct Stamp\nseat: Int\nend\n'
                        'fn open(size: Int) : Option(Pool)\n'
                        'if size >= 2: Some(Pool(size: size)) else: None\nend\n'
                        'fn issue(pool: Pool) : Ticket\nTicket(seat: pool.size)\nend\n'
                        'fn stamp(ticket: Ticket) : Stamp\nStamp(seat: ticket.seat)\nend\n'
                        'fn seat_of(stamp: Stamp) : Int\npromise "under seven"\nresult < 7\n'
                        'allows stamp: Stamp(seat: 2), result: 2\n'
                        'forbids stamp: Stamp(seat: 2), result: 7\nend\nstamp.seat\nend\n' + tail)
        items = records(run(binary, ['test', '--json', str(path)], 1), 'test')
        assert items[1]['input'] == [{'name': 'stamp', 'value': 'Stamp(seat: 7)'}], items
        assert items[1]['result'] == '7' and items[-1]['errors'] == 0, items
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

        source = path.read_text().replace('expose A, B, make', 'expose A, B, C, make, start')
        source = source.replace('fn apart(a: A, b: B)', 'fn apart(a: A, b: B, extra: C)')
        source = source.replace('result: true', 'extra: C(value: 7), result: true')
        source = source.replace('result: false', 'extra: C(value: 7), result: false')
        source = source.replace('a.n != b.n or a.m != b.m',
                                'a.n != b.n or a.m != b.m or extra.value < 7')
        path.write_text(source + 'struct C\nvalue: Int\nend\n'
                        'fn start(value: Int) : Option(C)\n'
                        'if value >= 2: Some(C(value: value)) else: None\nend\n')
        items = records(run(binary, ['test', '--json', str(path)], 1), 'test')
        assert items[1]['outcome'] == 'failed', items
        a, b, extra = (entry['value'] for entry in items[1]['input'])
        assert a[1:] == b[1:] and a[0] == 'A' and b[0] == 'B', items
        assert extra == 'C(value: 7)' and items[-1]['errors'] == 0, items

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
