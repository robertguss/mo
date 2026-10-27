#!/usr/bin/env python3
"""D306 source contracts through the CLI and the real service/store implementation."""
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile


TIME = 'Time.parse("2026-01-01T00:00:00Z") or stop "time"'
SERVICE = '''module Main
service
  state Int = 0
  tools snapshot
  every 1.minute tick
  token from env "TOKEN"
end
fn tick(state: Int, _now: Time) : Int
  state + 1
end
fn snapshot(state: Int, _now: Time) : List(Int)
  [state]
end
fn main() : Int
  0
end
'''


def results(binary, path, status=0, *flags):
    result = subprocess.run([str(binary), 'test', '--json', *flags, str(path)],
                            capture_output=True, text=True, timeout=60)
    assert result.returncode == status and not result.stderr, result
    records = [json.loads(line) for line in result.stdout.splitlines()]
    assert not any(r['record'] == 'diagnostic' for r in records), records
    return [r for r in records if r['record'] == 'result']


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    checked = subprocess.run([str(binary), 'check',
                              'tests/lower-source/service-simulation/controls.mo'],
                             capture_output=True, text=True, timeout=60)
    assert checked.returncode == 0 and not checked.stdout and not checked.stderr, checked
    passed = results(binary, 'tests/lower-source/service-simulation/main.mo')
    assert len(passed) == 9 and all(r['outcome'] == 'passed' for r in passed), passed
    with tempfile.TemporaryDirectory(prefix='mo-simulation-') as directory:
        root = pathlib.Path(directory)
        path = root / 'main.mo'
        shutil.copyfile('examples/job-queue.mo', root / 'job-queue.mo')
        for file in pathlib.Path('tests/service-simulation').glob('*.mo'):
            shutil.copyfile(file, root / file.name)
        jobs = [r for i in (1, 2) for r in
                results(binary, path, 0, '--only', f'Main.test.{i}')]
        assert len(jobs) == 2 and all(r['outcome'] == 'passed' for r in jobs), jobs

        path.write_text(SERVICE + f'''
test "exactly 10000 scheduled firings succeed"
  sim = Simulation.start(Main, {TIME})
  Simulation.advance(sim, 10000.minutes)
  assert Simulation.send(sim, Main.snapshot) == Ok([10001])
end
''')
        assert results(binary, path)[0]['outcome'] == 'passed'

        paired = SERVICE.replace('every 1.minute tick',
                                 'every 1.minute tick\n  every 1.minute second')
        paired += '\nfn second(state: Int, _now: Time) : Int\nstate + 10\nend\n'
        path.write_text(paired + f'''test "simultaneous firings count separately"
sim = Simulation.start(Main, {TIME})
Simulation.advance(sim, 5000.minutes)
assert Simulation.send(sim, Main.snapshot) == Ok([55011])
end
test "two simultaneous firings exceed the limit"
sim = Simulation.start(Main, {TIME})
Simulation.advance(sim, 5001.minutes)
end
''')
        paired_results = results(binary, path, 1)
        assert paired_results[0]['outcome'] == 'passed', paired_results
        assert paired_results[1]['outcome'] == 'stopped', paired_results
        assert '10002 firings' in str(paired_results[1]), paired_results

        body = SERVICE
        failures = [
            ('Simulation.advance(sim, 10001.minutes)', '10001 firings', '10000'),
            ('Simulation.advance(sim, -1.second)', 'nonnegative', '9999'),
            ('Simulation.advance(sim, 9223372036854.seconds)', 'nonnegative', '9999'),
            ('Simulation.restart(sim)', 'restart', 'crashed or stopped'),
        ]
        for i in range(12):
            statement, _, _ = failures[i % len(failures)]
            if i in (4, 9):
                statement = ('assert Simulation.send(sim, Main.snapshot) == Ok([0])\n'
                             'Simulation.crash(sim)')
            body += f'\ntest "stop {i}"\nsim = Simulation.start(Main, {TIME})\n{statement}\nend\n'
        body += f'''test "store slots were released"
sim = Simulation.start(Main, {TIME})
assert Simulation.send(sim, Main.snapshot) == Ok([1])
end
'''
        path.write_text(body)
        stopped = results(binary, path, 1)
        assert len(stopped) == 13 and stopped[-1]['outcome'] == 'passed', stopped
        for i, record in enumerate(stopped[:-1]):
            if i in (4, 9):
                assert record['outcome'] == 'failed', record
                continue
            assert record['outcome'] == 'stopped', record
            rendered = str(record)
            for text in failures[i % len(failures)][1:]:
                assert text in rendered, (text, record)

        for source, prefix, reason in (
            (SERVICE.replace('state Int = 0', 'state Int = initial()') +
             '\nfn initial() : Int\nstop "initial"\nend\n',
             'Simulation.start at 2026-01-01T00:00:00Z: start value', 'initial'),
            (SERVICE.replace('state + 1', 'if state == 0: stop "boom" else: state'),
             'Simulation handler Main.tick at 2026-01-01T00:00:00Z', 'boom'),
        ):
            path.write_text(source + f'\ntest "startup failure"\n_sim = Simulation.start(Main, {TIME})\nend\n')
            stopped = results(binary, path, 1)
            assert stopped[0]['outcome'] == 'stopped', stopped
            expected = f'{prefix}: service handler failed: stop: {reason}'
            assert expected in str(stopped[0]), stopped

        path.write_text(SERVICE.replace('state + 1',
                        'if state == 1: stop "later" else: state + 1') + f'''
test "deadline failure"
sim = Simulation.start(Main, {TIME})
Simulation.advance(sim, 1.minute)
end
''')
        stopped = results(binary, path, 1)
        assert ('Simulation handler Main.tick at 2026-01-01T00:01:00Z: '
                'service handler failed: stop: later') in str(stopped[0]), stopped

        path.write_text(SERVICE.replace('module Main', 'module Main\nuse Ticks') + f'''
test "wrong service"
sim = Simulation.start(Main, {TIME})
_reply = Simulation.send(sim, Ticks.history)
end
''')
        shutil.copyfile('tests/lower-source/service-simulation/ticks.mo', root / 'ticks.mo')
        stopped = results(binary, path, 1)
        assert ('Simulation.send selected Ticks.history, but this simulation runs Main; '
                'nothing was queued or committed') in str(stopped[0]), stopped

        transfers = '''module Main
enum Chain
  End
  More(Chain)
end
fn chain(n: Int) : Chain
  if n == 0: End else: More(chain(n - 1))
end
service
  state Int = 0
  tools input, output, rejection
  token from env "TOKEN"
end
fn input(_state: Int, _now: Time, _value: Chain) : Bool
  true
end
fn output(_state: Int, _now: Time) : Chain
  chain(130)
end
fn rejection(_state: Int, _now: Time) : Result(Bool, Chain)
  Error(chain(130))
end
fn main() : Int
  0
end
'''
        for call, direction, detail in (
            ('Main.input, chain(130)', 'copy-in', 'nothing was queued or committed'),
            ('Main.output', 'copy-out', 'tool output executed'),
            ('Main.rejection', 'copy-out', 'tool rejection executed'),
        ):
            path.write_text(transfers + f'''test "saved depth boundary"
sim = Simulation.start(Main, {TIME})
_result = Simulation.send(sim, {call})
end
''')
            stopped = results(binary, path, 1)
            assert stopped[0]['outcome'] == 'stopped', stopped
            for text in (direction, detail, 'MO_LIMIT_SAVED_DEPTH', '128'):
                assert text in str(stopped[0]), (text, stopped)
    print('Simulation: lifecycle, deadlines, D308, real JobQueue recovery, cleanup and transfer limits pass')


if __name__ == '__main__':
    main()
