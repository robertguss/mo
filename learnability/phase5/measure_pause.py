#!/usr/bin/env python3
"""Production pause driver (development unless frozen); each sample keeps its source, store and raw logs."""
import argparse
import hashlib
import http.client
import json
import os
from pathlib import Path
import platform
import select
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
STAGES = dict.fromkeys(('output_encode validation_decode '
                       'timer_validation live_admit protocol_catalog swap_prepare '
                       'activation_report').split(), 'ok')
STAGES.update(identity_decode='ok_no_migration', cached_state='cache_hit_no_encoding', boundary_to_commit='committed',
              store_activate='committed')


def parse_records(text):
    records = [json.loads(line) for line in text.splitlines() if line.startswith('{"mo_pause":')]
    active = [r for r in records if r['attempt'] > 0]
    if not active or len({r['attempt'] for r in active}) != 1:
        raise ValueError('missing or ambiguous activation attempt')
    for r in active:
        if type(r.get('elapsed_ns')) is not int or r['elapsed_ns'] < 0:
            raise ValueError('missing/null/invalid duration')
    if any(r['stage'] in ('old_decode', 'migration_call') for r in active):
        raise ValueError('explicit migration is not unchanged-schema identity transfer')
    for stage, outcome in STAGES.items():
        matches = [r for r in active if r['stage'] == stage]
        if len(matches) != 1 or matches[0]['outcome'] != outcome:
            raise ValueError(f'missing/duplicate stage or wrong outcome: {stage}')
    return active


def fixture(folder, jobs, leases):
    if not 0 <= leases <= jobs <= 5000:
        raise ValueError('invalid fixture counts')
    folder.mkdir()
    main = (ROOT / 'services/task-queue/main.mo').read_text()
    queue = (ROOT / 'services/task-queue/job-queue.mo').read_text()
    queue = queue.replace('empty,', 'empty,\nseeded,\nverify_fixture,', 1)
    queue += f'''\nfn seeded() : Queue
  var jobs = empty.jobs
  until = Time.parse("2099-01-01T00:00:00Z") or stop "fixture time"
  for id in 1..{jobs + 1}
    prefix = "job-#{{id}}:"
    payload = "#{{prefix}}#{{"q".repeat(2048 - prefix.byte_count)}}"
    state = if id <= {leases} : Leased(until: until) else: Ready
    jobs = jobs.set(id, Job(id: id, payload: payload, attempts: if id <= {leases} : 1 else: 0, state: state))
  end
  Queue(jobs: jobs, next_id: {jobs + 1})
end
'''
    queue += f'''\nfn verify_fixture(queue: Queue) : Bool
  if queue.next_id != {jobs + 1} or queue.jobs.size != {jobs}
    return false
  end
  until = Time.parse("2099-01-01T00:00:00Z") or stop "fixture time"
  for id in 1..{jobs + 1}
    job = queue.jobs.get(id) or stop "missing fixture job"
    prefix = "job-#{{id}}:"
    payload = "#{{prefix}}#{{"q".repeat(2048 - prefix.byte_count)}}"
    state = if id <= {leases} : Leased(until: until) else: Ready
    attempts = if id <= {leases} : 1 else: 0
    if job.id != id or job.payload != payload or job.attempts != attempts or job.state != state
      return false
    end
  end
  true
end
'''
    main += '\nfn verify_fixture(queue: Queue) : Bool\n  JobQueue.verify_fixture(queue)\nend\n'
    main = main.replace('fail, list, stats', 'fail, list, stats, inspect_fixture')
    main += '\nfn inspect_fixture(queue: Queue, _now: Time) : Bool\n  verify_fixture(queue)\nend\n'
    main = main.replace('state Queue = JobQueue.empty', 'state Queue = JobQueue.seeded()')
    (folder / 'job-queue.mo').write_text(queue)
    (folder / 'main.mo').write_text(main)
    return main


def read_jobs(port, count, leases):
    found = []
    while len(found) < count:
        time.sleep(.055)
        conn = http.client.HTTPConnection('127.0.0.1', port, timeout=60)
        body = {'jsonrpc': '2.0', 'id': 1, 'method': 'tools/call', 'params': {
            'name': 'list', 'arguments': {'filter': {'case': 'AllJobs'},
                                        'after': str(len(found)), 'limit': '4'}}}
        try:
            conn.request('POST', '/mcp', json.dumps(body), {
                'Authorization': 'Bearer PauseMeasurementServiceCanary',
                'Content-Type': 'application/json', 'MCP-Protocol-Version': '2025-11-25'})
            reply = json.loads(conn.getresponse().read())['result']
            if reply['isError']:
                raise ValueError(reply)
            page = reply['structuredContent']['value']['jobs']
            if not page:
                raise ValueError('missing fixture jobs')
            found.extend(page)
        finally:
            conn.close()
    if len(found) != count or sum(j['state']['case'] == 'Leased' for j in found) != leases:
        raise ValueError('fixture counts differ')
    for i, job in enumerate(found, 1):
        prefix = f'job-{i}:'
        if job['id'] != str(i) or job['payload'] != prefix + 'q' * (2048 - len(prefix)):
            raise ValueError('fixture payload differs')
    return found


def verify_live(port, folder, label):
    """Run the frozen fixture verifier outside the activation interval."""
    conn = http.client.HTTPConnection('127.0.0.1', port, timeout=120)
    body = {'jsonrpc': '2.0', 'id': 1, 'method': 'tools/call',
            'params': {'name': 'inspect_fixture', 'arguments': {}}}
    try:
        conn.request('POST', '/mcp', json.dumps(body), {
            'Authorization': 'Bearer PauseMeasurementServiceCanary',
            'Content-Type': 'application/json', 'MCP-Protocol-Version': '2025-11-25'})
        response = conn.getresponse()
        raw = response.read()
        (folder / (label + '-verify.json')).write_bytes(raw)
        result = json.loads(raw).get('result', {})
        if response.status != 200 or result.get('isError') is not False or result.get('structuredContent') != {'value': True}:
            raise ValueError(label + ' fixture state differs')
        return result
    finally:
        conn.close()


def verify_snapshot(binary, port, source, folder, label, env):
    """Check the exported state exactly, outside the production activation pause."""
    artifact = folder / (label + '.mosx')
    commands = [
        ('fetch', [str(binary), 'fetch', f'http://127.0.0.1:{port}/mcp', '--export',
                   'state', '--out', str(artifact), '--token-env', 'QUEUE_TOKEN']),
        ('query', [str(binary), 'query', str(source), 'verify_fixture', '--export',
                   str(artifact), '--json'])]
    result = None
    for name, command in commands:
        result = captured(command, folder, label + '-' + name, env, timeout=1800)
        if result.returncode:
            raise ValueError(f'{label} {name} failed; raw output retained')
    records = [json.loads(line) for line in result.stdout.splitlines()]
    queries = [r for r in records if r.get('record') == 'query' and 'outcome' in r]
    if len(queries) != 1 or queries[0].get('outcome') != 'returned' or queries[0].get('result') != {'value': True, 'complete': True}:
        raise ValueError(f'{label} fixture state differs')
    return {'artifact_sha256': digest(artifact), 'query': queries[0]}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def captured(command, folder, name, env, timeout=300):
    started = time.monotonic()
    record = {'command': command, 'timeout_seconds': timeout}
    try:
        result = subprocess.run(command, capture_output=True, env=env, cwd=ROOT, timeout=timeout)
        (folder / (name + '.stdout')).write_bytes(result.stdout)
        (folder / (name + '.stderr')).write_bytes(result.stderr)
        record['exit_status'] = result.returncode
        return result
    except subprocess.TimeoutExpired as exc:
        (folder / (name + '.stdout')).write_bytes(exc.stdout or b'')
        (folder / (name + '.stderr')).write_bytes(exc.stderr or b'')
        record.update(timed_out=True, failure=str(exc))
        raise
    except Exception as exc:
        record['failure'] = str(exc)
        raise
    finally:
        record['elapsed_seconds'] = time.monotonic() - started
        (folder / (name + '.command.json')).write_text(json.dumps(record, indent=2) + '\n')


def run(command, folder, name, env):
    p = captured(command, folder, name, env)
    if p.returncode:
        raise RuntimeError(f'{name} exited {p.returncode}; raw output retained')
    return json.loads(p.stdout)


def sample(binary, folder, jobs, leases):
    folder.mkdir()
    main = fixture(folder / 'old', jobs, leases)
    fixture(folder / 'new', jobs, leases)
    (folder / 'new/main.mo').write_text(main + '\n# Unchanged-schema identity candidate.\n')
    env = {**os.environ, 'QUEUE_TOKEN': 'PauseMeasurementServiceCanary',
           'QUEUE_UPGRADE': 'PauseMeasurementUpgradeCanary'}
    base = run([str(binary), 'check', str(folder / 'old/main.mo'), '--upgrade-from',
                str(folder / 'old/main.mo'), '--json'], folder, 'check', env)['candidate_source']
    (folder / 'data').mkdir()
    with (folder / 'serve.stderr').open('wb') as err:
        p = subprocess.Popen([str(binary), 'serve', str(folder / 'old/main.mo'), '--port', '0',
                              '--data', str(folder / 'data'), '--upgrade-token-env', 'QUEUE_UPGRADE'],
                             stdout=subprocess.PIPE, stderr=err, env=env, cwd=ROOT)
        try:
            if not select.select([p.stdout], [], [], 60)[0]:
                raise RuntimeError('serve startup timeout')
            line = p.stdout.readline()
            (folder / 'serve.stdout').write_bytes(line)
            port = int(line.split(b':')[-1].split(b'/')[0])
            before = verify_live(port, folder, 'before')
            report = run([str(binary), 'push', str(folder / 'new/main.mo'), '--url',
                          f'http://127.0.0.1:{port}/mcp', '--base', base,
                          '--token-env', 'QUEUE_TOKEN', '--upgrade-token-env', 'QUEUE_UPGRADE',
                          '--json'], folder, 'push', env)
            after = verify_live(port, folder, 'after')
            (folder / 'verified-state.json').write_text(json.dumps({'before': before, 'after': after}))
        finally:
            p.terminate()
            try:
                p.wait(timeout=30)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
            p.stdout.close()
    records = parse_records((folder / 'serve.stderr').read_text())
    return {'report': report, 'records': records,
            'source_hashes': {str(f.relative_to(folder)): digest(f) for f in folder.rglob('*.mo')}}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', action='append', required=True, help='LABEL=/absolute/mo; debug and optimized for later collection')
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--repetitions', type=int, default=5)
    ap.add_argument('--jobs', type=int, default=5000)
    ap.add_argument('--leases', type=int, default=500)
    ap.add_argument('--build-description', required=True)
    ap.add_argument('--freeze', type=Path, help='verified immutable manifest for final collection')
    args = ap.parse_args()
    if args.repetitions < 1 or not 0 <= args.leases <= args.jobs <= 5000:
        ap.error('positive repetitions and 0 <= leases <= jobs <= 5000 required')
    frozen = None
    if args.freeze:
        frozen = json.loads(args.freeze.read_text())
        if (args.jobs, args.leases, args.repetitions) != (5000, 500, 5):
            ap.error('final workload is fixed at 5000 jobs, 500 leases, five repetitions')
        binaries = dict(spec.split('=', 1) for spec in args.binary)
        if set(binaries) != {'debug', 'optimized'} or len(args.binary) != 2:
            ap.error('final collection requires exactly debug and optimized binaries')
        if subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT).decode().strip() != frozen['candidate_commit']:
            ap.error('candidate differs from freeze')
        subprocess.run(['git', 'merge-base', '--is-ancestor', frozen['repair_commit'],
                        frozen['candidate_commit']], cwd=ROOT, check=True)
        files = {name: sha for group in frozen['artifacts'].values() for name, sha in group.items()}
        for name, sha in files.items():
            if digest(Path(name)) != sha:
                ap.error('frozen artifact changed: ' + name)
        for label, filename in binaries.items():
            if files.get(str(Path(filename).resolve())) != digest(Path(filename)):
                ap.error('measurement binary absent from freeze')
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    result = {'label': 'FINAL' if frozen else 'DEVELOPMENT',
              'freeze_sha256': digest(args.freeze) if frozen else None, 'host': platform.uname()._asdict(),
              'cpu_models': sorted({line.split(':', 1)[1].strip() for line in Path('/proc/cpuinfo').read_text().splitlines() if line.startswith('model name')}),
              'memory_total': next(line for line in Path('/proc/meminfo').read_text().splitlines() if line.startswith('MemTotal:')),
              'physical_storage': 'not exposed by the exe.dev guest; guest device/filesystem disclosed below',
              'build_description': args.build_description,
              'build_files_sha256': {str(p.relative_to(ROOT)): digest(p) for p in
                                     [ROOT / 'Makefile', *sorted((ROOT / 'src').rglob('*'))]
                                     if p.is_file()}, 'target_seconds': 1, 'required_margin': 2,
              'jobs': args.jobs, 'leases': args.leases, 'payload_bytes': 2048, 'samples': [],
              'filesystem': subprocess.run(['findmnt', '-T', str(args.output), '-J'], capture_output=True, text=True).stdout,
              'sync': 'production SQLite journal_mode=DELETE, synchronous=EXTRA, temp_store=MEMORY; synchronous probe stderr writes included',
              'separate_compiler_stall_evidence': 'not exercised by pause workload; see separately retained compiler-family controls'}
    try:
        for spec in args.binary:
            label, path = spec.split('=', 1)
            binary = Path(path).resolve()
            for n in range(args.repetitions):
                item = {'label': label, 'binary_sha256': digest(binary), 'index': n}
                result['samples'].append(item)
                try:
                    item.update(sample(binary, args.output / f'{label}-{n}', args.jobs, args.leases))
                except Exception as exc:
                    item['failure'] = str(exc)
        if any('failure' in s for s in result['samples']):
            raise RuntimeError('one or more samples failed; all attempts retained')
        result['summary'] = {}
        for label in {s['label'] for s in result['samples']}:
            stages = {}
            for stage in STAGES:
                values = [r['elapsed_ns'] / 1e9 for sample in result['samples']
                          if sample['label'] == label for r in sample['records']
                          if r['stage'] == stage]
                stages[stage] = dict(count=len(values), min=min(values),
                                     median=statistics.median(values), max=max(values),
                                     sample_stddev=statistics.stdev(values) if len(values) > 1 else None)
            result['summary'][label] = {
                'stages_seconds': stages,
                'meets_margin': stages['boundary_to_commit']['max'] <= .5}

    except Exception as exc:
        result['failure'] = str(exc)
        raise
    finally:
        (args.output / 'results.json').write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
