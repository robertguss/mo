"""Operator-only real queue fixture; never install this module in a session home."""
import os
import json
import pathlib
import shutil
import subprocess
import proxy

base = proxy.load('fixture')
ROOT = pathlib.Path(__file__).resolve().parents[3]
Client = base.Client


class Service(base.Service):
    def __init__(self, mo, data, token, upgrade, log, source=None):
        pathlib.Path(data).mkdir(mode=0o700, parents=True, exist_ok=True)
        self.stderr_path = pathlib.Path(log)
        self.stderr = self.stderr_path.open('wb')
        self.process = subprocess.Popen(
            [str(mo), 'serve', *([str(source)] if source else []), '--data', str(data),
             '--port', '0', '--upgrade-token-env', 'QUEUE_UPGRADE'],
            env={**os.environ, 'QUEUE_TOKEN': token, 'QUEUE_UPGRADE': upgrade},
            stdout=subprocess.PIPE, stderr=self.stderr, stdin=subprocess.DEVNULL, bufsize=0)
        try:
            self.port = self.listening()
            self.url = f'http://127.0.0.1:{self.port}/mcp'
        except BaseException:
            self.process.kill()
            self.process.wait()
            self.process.stdout.close()
            self.stderr.close()
            raise


def sources(out, control='reference'):
    out = pathlib.Path(out)
    shutil.copytree(ROOT / 'services/task-queue', out)
    if control in ('unchanged', 'source-only'):
        return out
    main = (out / 'main.mo').read_text().replace('use JobQueue', 'use UpgradeOld\nuse JobQueue')
    main = main.replace('fail, list, stats', 'fail, list, stats, revision')
    main += '\nfn revision(queue: Queue, _now: Time) : Int\n  JobQueue.revision(queue)\nend\n'
    migration = (pathlib.Path(__file__).parent / 'fixtures/migration.mo.inc').read_text()
    if control == 'initializer':
        migration = migration[:migration.index('  jobs =')].replace('migrate(old:', 'migrate(_old:') + '  JobQueue.empty\nend\n'
    elif control == 'drop-job':
        migration = migration.replace('old.jobs.values.map', 'old.jobs.values.drop(1).map')
    elif control == 'payload':
        migration = migration.replace('payload: job.payload', 'payload: "altered"')
    elif control == 'duplicate-completion':
        migration = migration.replace('UpgradeOld.Leased(until: until): JobQueue.Leased(until: until)',
                                      'UpgradeOld.Leased(until: _): JobQueue.Done')
    elif control == 'wrong-next-id':
        migration = migration.replace('old.next_id, 1)', 'old.next_id + 1, 1)')
    elif control == 'constant-revision':
        main = main.replace('fn revision(queue: Queue, _now: Time) : Int\n  JobQueue.revision(queue)',
                            'fn revision(_queue: Queue, _now: Time) : Int\n  1')
        migration = migration.replace('old.next_id, 1)', 'old.next_id)')
    elif control == 'wrong-revision':
        migration = migration.replace('old.next_id, 1)', 'old.next_id, 9)')
    (out / 'main.mo').write_text(main + migration)
    queue = out / 'job-queue.mo'
    if control == 'constant-revision':
        body = queue.read_text().replace('expose Queue,', 'expose Queue, restore,')
        body += '\nfn restore(jobs: Map(Int, Job), next_id: Int) : Queue\n  Queue(jobs: jobs, next_id: next_id)\nend\n'
        queue.write_text(body)
        return out
    body = queue.read_text().replace('expose Queue,', 'expose Queue, restore, revision,')
    body = body.replace('  next_id: Int\n', '  next_id: Int\n  revision: Int\n')
    body = body.replace('Queue(jobs:', 'Queue(revision: 1, jobs:')
    body += '\nfn restore(jobs: Map(Int, Job), next_id: Int, revision: Int) : Queue\n  Queue(jobs: jobs, next_id: next_id, revision: revision)\nend\n'
    body += '\nfn revision(queue: Queue) : Int\n  queue.revision\nend\n'
    if control == 'qualified-reference':
        body = body.replace('\n\nmax_attempts', '\n\nuse UpgradeOld\n\nmax_attempts', 1)
        body = body.replace('expose Queue,', 'expose Queue, migrate,', 1)
        body += migration.replace('JobQueue.', '')
        (out / 'main.mo').write_text(main.replace('use UpgradeOld\n', ''))
    queue.write_text(body)
    return out


def populate(client, stable_lease=False):
    for payload in ('completed item', 'leased payload λ', 'ready item', 'another ready item'):
        client.ok('submit', payload=payload)
    job = client.ok('lease')['value']
    client.ok('acknowledge', id=job['id'], attempt=job['attempts'])
    job = client.ok('lease')['value']
    client.ok('fail', id=job['id'], attempt=job['attempts'])
    client.ok('fixture_lease' if stable_lease else 'lease')
    return {'jobs': client.every_job(), 'revision': '1', 'stored_revision': '1', 'next_id': '5'}


def precheck(client, expected):
    state = client.ok('mo-state')
    return (client.every_job() == expected['jobs'] and state.get('complete') is True and
            state.get('state', {}).get('fields', {}).get('next_id') == expected['next_id'])


def build(mo, out, token, upgrade):
    """Create protected populated folder and retained historical source for a runner.

    Copy only old/ to the session; data/ and expected.json remain operator-owned.
    A temporary tool seeds the final lease at 2099, then a tool-only upgrade
    restores the exact original source. No historical migration dependency is
    introduced. The one preparation activation is frozen separately from the
    session's required activation.
    """
    import json
    out = pathlib.Path(out)
    out.mkdir(mode=0o700, parents=True, exist_ok=False)
    old = sources(out / 'old', 'unchanged')
    preparation = out / 'preparation'
    shutil.copytree(old, preparation)
    main = preparation / 'main.mo'
    main.write_text(main.read_text().replace('fail, list, stats',
                                            'fail, list, stats, fixture_lease') +
        '\nfn fixture_lease(queue: Queue, _now: Time) : Result((Queue, Option(Job)), QueueError)\n'
        '  at = Time.parse("2099-01-01T00:00:00Z") or stop "fixture time"\n'
        '  JobQueue.lease(queue, at, lease_length)\nend\n')
    service = Service(mo, out / 'data', token, upgrade, out / 'service.log', main)
    try:
        client = Client(service.url, token)
        expected = populate(client, stable_lease=True)
        checked = subprocess.run([str(mo), 'check', str(old / 'main.mo'),
            '--upgrade-from', str(main), '--json'], capture_output=True, text=True,
            timeout=120, check=True)
        (out / 'preparation-check.json').write_text(checked.stdout)
        identity = json.loads(checked.stdout)
        pushed = subprocess.run([str(mo), 'push', str(old / 'main.mo'), '--url', service.url,
            '--base', identity['base_source'], '--token-env', 'QUEUE_TOKEN',
            '--upgrade-token-env', 'QUEUE_UPGRADE', '--json'],
            env={**os.environ, 'QUEUE_TOKEN': token, 'QUEUE_UPGRADE': upgrade},
            capture_output=True, text=True, timeout=180, check=True)
        (out / 'preparation-push.json').write_text(pushed.stdout)
        report = json.loads(pushed.stdout)
        if (report['migration'] is not None or report['ordinal'] != '1' or
                report['old_schema'] != report['new_schema'] or not precheck(client, expected)):
            raise ValueError('fixture preparation changed state or required migration')
    finally:
        service.stop()
    result = subprocess.run([str(mo), 'history', '--json', str(out / 'data')],
                            capture_output=True, text=True, timeout=120, check=True)
    (out / 'preparation-history.json').write_text(result.stdout)
    recorded = next(r for r in map(json.loads, result.stdout.splitlines()) if r.get('record') == 'history')
    if recorded.get('program', {}).get('source') != identity['candidate_source'] or len(recorded['upgrades']) != 1:
        raise ValueError('fixture preparation identity mismatch')
    expected['preparation_upgrades'] = recorded['upgrades']
    expected['lease_fixture_time'] = '2099-01-01T00:00:00Z'
    path = out / 'expected.json'
    path.write_text(json.dumps(expected, indent=2) + '\n')
    path.chmod(0o600)
    return expected
