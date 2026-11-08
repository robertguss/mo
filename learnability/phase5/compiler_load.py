#!/usr/bin/env python3
"""Development production bundle-compiler controls; retain every attempted input."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

BASE = '''module Main
service
  state Int = 0
  tools read
  token from env "LOAD_TOKEN"
end
fn read(state: Int, _now: Time) : Bool
  state == 0
end
'''


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def collect(binary, out):
    out.mkdir(parents=True, exist_ok=False)
    (out / 'old').mkdir()
    old = out / 'old/main.mo'
    old.write_text(BASE)
    records = []
    def check(name, text):
        folder = out / name
        folder.mkdir()
        source = folder / 'main.mo'
        source.write_text(text)
        command = [str(binary), 'check', str(source), '--upgrade-from', str(old), '--json']
        started = time.monotonic()
        record = dict(name=name, command=command, source_sha256=sha(source))
        records.append(record)
        result = None
        try:
            result = subprocess.run(command, capture_output=True, timeout=60)
            (folder / 'stdout').write_bytes(result.stdout)
            (folder / 'stderr').write_bytes(result.stderr)
            record['exit_status'] = result.returncode
            reports = [json.loads(line) for line in result.stdout.splitlines()]
            record['reports'] = reports
        except subprocess.TimeoutExpired as exc:
            (folder / 'stdout').write_bytes(exc.stdout or b'')
            (folder / 'stderr').write_bytes(exc.stderr or b'')
            record.update(timeout=True, failure=str(exc))
            raise
        except Exception as exc:
            record['failure'] = str(exc)
            raise
        finally:
            record['seconds'] = time.monotonic() - started
            (out / 'attempts.json').write_text(json.dumps(records, indent=2) + '\n')
        if result.returncode not in (0, 1):
            raise RuntimeError('unexpected compiler process status')
        if len(reports) != 1:
            raise RuntimeError('unexpected upgrade report framing')
        report = reports[0]
        if result.returncode == 0:
            if (report.get('status') != 'accepted' or not report.get('candidate_source') or
                    not report.get('old_schema') or report.get('old_schema') != report.get('new_schema')):
                raise RuntimeError('accepted compilation lacks validated schema identity')
        elif name.startswith('nesting-') and (
                report.get('code') != 'E1419' or report.get('message') != 'code is nested too deeply'):
            raise RuntimeError('nesting refused for an unrelated reason')
        return result.returncode == 0
    generic = BASE.replace('  state == 0\nend', '\n'.join(
        f'  _v{depth} = identity(' + '[' * depth + '1' + ']' * depth + ')'
        for depth in range(1, 65)) + '\n  state == 0\nend')
    generic += '\nfn identity(value: T) : T\n  value\nend\n'
    if not check('generics-64', generic):
        raise RuntimeError('64-instantiation family refused; evidence retained')
    def nested(depth):
        return BASE.replace('  state == 0\nend', '  ' + '(' * depth + 'state == 0' + ')' * depth + '\nend')
    low, high = 0, 257
    if not check('nesting-0', nested(low)) or check('nesting-257', nested(high)):
        raise RuntimeError('declared nesting search bounds did not bracket acceptance')
    while high - low > 1:
        midpoint = (low + high) // 2
        if check(f'nesting-{midpoint}', nested(midpoint)):
            low = midpoint
        else:
            high = midpoint
    summary = dict(evidence_class='DEVELOPMENT', binary_sha256=sha(binary),
                   generic_instantiations=64, accepted_parentheses=low,
                   first_refused_parentheses=high, attempts=records,
                   production_child_limits=dict(memory_bytes=1073741824, cpu_seconds=10, wall_seconds=15),
                   limits='Family-specific compiler acceptance, not all type shapes or worst-case resource use. Timings include old and candidate compilation plus bundling; negative parsing can refuse before child launch.')
    (out / 'results.json').write_text(json.dumps(summary, indent=2) + '\n')
    return summary


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    result = collect(args.binary.resolve(), args.output.resolve())
    print(json.dumps({key: value for key, value in result.items() if key != 'attempts'}, indent=2))
