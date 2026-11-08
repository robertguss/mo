#!/usr/bin/env python3
"""Protected Phase5 collector. See runner-development.md for the private input contract."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import secrets
import shutil
import signal
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

# Resolve legacy bare imports only during loading, restoring the host interpreter.
def isolated(directory, names):
    saved_path = sys.path[:]
    saved = {name: sys.modules.get(name) for name in names}
    loaded = {}
    try:
        sys.path.insert(0, str(directory))
        for name in names:
            sys.modules.pop(name, None)
        for name in names:
            module = load(name, directory / (name + '.py'))
            sys.modules[name] = module
            loaded[name] = module
        return loaded
    finally:
        sys.path[:] = saved_path
        for name, module in saved.items():
            if module is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = module

p4 = isolated(HERE.parents[1] / 'phase4', ['fixture', 'proxy', 'grade', 'harness'])['harness']
local = isolated(HERE, ['proxy', 'fixture', 'grade'])
proxy, grader = local['proxy'], local['grade']
SLOTS = ['codex', 'claude', 'codex', 'claude']
VERSIONS = {'codex': 'codex-cli 0.166.0', 'claude': '2.1.317 (Claude Code)'}
BASE = 'd31c9301132ac9e55432bf9e36855c2914ecab21'
ENTER = p4.ENTER.replace('os.chdir', 'env["QUEUE_UPGRADE"] = os.environ["QUEUE_UPGRADE"]; os.chdir')


def layout():
    return p4.Layout(user='mo-phase5-session', install='/opt/mo-phase5',
                     state=Path.home() / '.mo-phase5-harness')


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()


def freeze(path, installed):
    f = json.loads(Path(path).read_text())
    root = f['repository']
    repair = f['repair_commit']
    if repair == BASE or len(repair) != 40:
        raise ValueError('a merged repair commit is required')
    for a, b in [(BASE, repair), (repair, f['main_commit']),
                 (repair, f['candidate_commit'])]:
        git(root, 'merge-base', '--is-ancestor', a, b)
    if git(root, 'rev-parse', 'refs/remotes/origin/main') != f['main_commit']:
        raise ValueError('main evidence differs from origin/main')
    if git(root, 'rev-parse', 'HEAD') != f['candidate_commit'] or git(root, 'status', '--porcelain'):
        raise ValueError('freeze requires exact clean candidate, including untracked inputs')
    required = {'protocol', 'prompt', 'harness', 'proxy', 'scanner', 'grader', 'fixtures',
                'controls', 'build_inputs', 'build_commands', 'compiler_versions', 'verification'}
    if not required <= f['artifacts'].keys() or f['slots'] != SLOTS:
        raise ValueError('incomplete freeze')
    for item in f['artifacts'].values():
        for filename, digest in item.items():
            if p4.sha256(filename) != digest:
                raise ValueError('frozen artifact changed')
    verification = json.loads(Path(f['verification_evidence']).read_text())
    if (f['verification_evidence'] not in f['artifacts']['verification'] or
            verification.get('commit') != repair or
            verification.get('independently_checked') is not True or
            verification.get('passed') is not True):
        raise ValueError('missing independently checked repair verification')
    controls = Path(f['controls_evidence'])
    if str(controls) not in f['artifacts']['controls']:
        raise ValueError('control evidence absent from freeze')
    proven = json.loads(controls.read_text())
    required_controls = ('real_service_grading', 'proxy_attestation', 'credential_redaction',
                         'collector_success', 'collector_timeout', 'source_tests')
    if any(proven.get(name) is not True for name in required_controls):
        raise ValueError('required development controls did not pass')
    for name in ('mo', 'codex', 'claude'):
        if p4.sha256(installed.bin / name) != f['binaries'][name]:
            raise ValueError('installed binary changed')
    return f


def check(installed):
    p4.check_private(installed, installed.state, HERE)
    for name, expected in VERSIONS.items():
        status, version = p4.as_session_user(installed, installed.bin / name, '--version')
        if status or version != expected:
            raise ValueError('frozen client version unavailable: ' + name)


def command(client, binary, prompt, url, config):
    cmd = p4.client_command(client, binary, prompt, url, config)
    if client == 'codex':
        cmd[-1:-1] = ['-c', 'mcp_servers.task-queue.env_http_headers={"Mo-Upgrade-Authorization"="QUEUE_UPGRADE"}']
    return cmd


def models(stream):
    found, events = set(), []
    def walk(x):
        if isinstance(x, dict):
            for k, v in x.items():
                if k == 'model' and isinstance(v, str):
                    found.add(v)
                walk(v)
        elif isinstance(x, list):
            for v in x:
                walk(v)
    for e in proxy.base.events(stream):
        walk(e)
        if 'model' in json.dumps(e).lower() or 'fallback' in json.dumps(e).lower():
            events.append(e)
    return {'reported_models': sorted(found), 'events': events}


def collect(l, spec, out, slot, development=False, limit=3600):
    if slot not in range(1, 5) or (not development and limit != 3600):
        raise ValueError('invalid slot or time limit')
    if limit <= 0:
        raise ValueError('positive time limit required')
    out = Path(out).resolve()
    out.mkdir(mode=0o700, parents=True, exist_ok=False)
    p4.check_private(l, out, Path(spec).resolve(), l.state)
    s = json.loads(Path(spec).read_text())
    for key in ('source', 'context', 'data'):
        p4.check_private(l, Path(s[key]))
        if any(p.is_symlink() for p in Path(s[key]).rglob('*')):
            raise ValueError('input trees must not contain symlinks')
    if not development:
        check(l)
        frozen = freeze(s['freeze'], l)
        # Every supplied input file must occur in the freeze, including this spec.
        hashes = {k: v for group in frozen['artifacts'].values() for k, v in group.items()}
        for top in [Path(spec), *[Path(s[k]) for k in ('source', 'context', 'data', 'prompt', 'expected')]]:
            for path in ([top] if top.is_file() else top.rglob('*')):
                if path.is_file() and hashes.get(str(path.resolve())) != p4.sha256(path):
                    raise ValueError('session input absent from freeze')
    expected = json.loads(Path(s['expected']).read_text())
    p4.check_private(l, Path(s['expected']), Path(s['experiment']))
    if expected.get('revision') != '1':
        raise ValueError('expected revision must be 1')
    p4.reset(l)
    token, upgrade = secrets.token_urlsafe(32), secrets.token_urlsafe(32)
    for value in (token, upgrade):
        p4.append_secret(l.tokens, value)
    meta = {'slot': slot, 'client': SLOTS[slot-1], 'evidence_class': 'DEVELOPMENT' if development else 'FINAL',
            'success': False, 'correct': False, 'development': development, 'launched': False, 'limit_seconds': limit, 'started_at': time.time(), 'failures': []}
    service = recorder = child = None
    firewall = False
    rule = None
    old = {sig: signal.signal(sig, p4.stopped) for sig in (signal.SIGTERM, signal.SIGHUP)}
    try:
        shutil.copytree(s['source'], out / 'serve')
        shutil.copytree(s['data'], out / 'start-data')
        shutil.copytree(s['data'], out / 'data')
        p4.place_read_only(s['context'], l.deployed)
        shutil.copytree(s['source'], l.work)
        p4.sudo('chown', '-R', f'{l.user}:{l.user}', l.work)
        env = {'PATH': '/usr/bin:/bin', 'QUEUE_TOKEN': token, 'QUEUE_UPGRADE': upgrade}
        service = p4.fixture.Service.__new__(p4.fixture.Service)
        service.stderr_path = l.service_log
        service.stderr = open(service.stderr_path, 'wb')
        os.chmod(service.stderr_path, 0o444)
        service.process = subprocess.Popen([str(l.bin / 'mo'), 'serve', 'main.mo', '--port', '0',
                                           '--data', str(out / 'data'), '--upgrade-token-env', 'QUEUE_UPGRADE'],
                                          cwd=out / 'serve', env=env,
                                          stdout=subprocess.PIPE, stderr=service.stderr, bufsize=0)
        port = service.listening()
        client = grader.fixture.Client(f'http://127.0.0.1:{port}/mcp', token)
        if not grader.precheck(client, expected):
            raise ValueError('fixture precheck failed before launch')
        # Deny the session uid even if it discovers the direct port with ss/ps.
        rule = ['OUTPUT', '-p', 'tcp', '--dport', str(port), '-m', 'owner',
                '--uid-owner', str(l.account().pw_uid), '-j', 'REJECT']
        p4.sudo('iptables', '-I', *rule)
        firewall = True
        recorder = proxy.Proxy(f'http://127.0.0.1:{port}/mcp', out / 'proxy.jsonl', token, upgrade,
                               list(set(l.secrets.read_text().split())) if l.secrets.exists() else ())
        l.mcp_config.write_text(json.dumps({'mcpServers': {p4.SERVER: {'type': 'http', 'url': recorder.url,
            'headers': {'Authorization': 'Bearer ${QUEUE_TOKEN}',
                        'Mo-Upgrade-Authorization': '${QUEUE_UPGRADE}'}}}}))
        os.chmod(l.mcp_config, 0o444)
        prompt = Path(s['prompt']).read_text().replace('SERVICE_URL', recorder.url)
        prompt += '\nUse the migration function name migrate. Your final message must be exactly 1.\n'
        meta['answer_rule'] = 'last assistant message, stripped, exactly 1'
        meta['migration_requirement'] = 'migrate'
        (out / 'prompt.txt').write_text(prompt)
        cmd = command(meta['client'], l.bin / meta['client'], prompt, recorder.url, l.mcp_config)
        environment = {'HOME': str(l.home()), 'USER': l.user, 'LOGNAME': l.user,
                       'SHELL': '/bin/bash', 'PATH': f'{l.bin}:/usr/bin:/bin',
                       'LANG': 'C.UTF-8', 'DISABLE_AUTOUPDATER': '1'}
        start = time.monotonic()
        with open(out / 'stream.jsonl', 'wb') as stdout, open(out / 'client-stderr.txt', 'wb') as stderr:
            reserve(s['experiment'], slot, out)
            meta['launched'] = True
            child = subprocess.Popen(['sudo', '-n', '-u', l.user,
                '--preserve-env=QUEUE_TOKEN,QUEUE_UPGRADE', '--', '/usr/bin/python3', '-I', '-c', ENTER,
                json.dumps(environment), str(l.work), *cmd], env=env, stdin=subprocess.DEVNULL,
                stdout=stdout, stderr=stderr, start_new_session=True)
            try:
                meta['exit_status'] = child.wait(timeout=limit)
                meta['timeout'] = False
            except subprocess.TimeoutExpired:
                meta['timeout'] = True
                p4.end_all(l.user)
                meta['exit_status'] = child.wait(timeout=30)
        meta['seconds'] = time.monotonic() - start
        meta['models'] = models(out / 'stream.jsonl')
        meta['live'] = grader.live_grade(client)
        if meta['timeout'] or meta['exit_status'] != 0:
            meta['failures'].append('timeout or unsuccessful exit')
        wanted = 'gpt-6.1-sol' if meta['client'] == 'codex' else 'claude-opus-5-5'
        if meta['models']['reported_models'] != [wanted] or any(e.get('fallback') for e in meta['models']['events']):
            meta['failures'].append('missing or substituted model evidence')
    except BaseException as error:
        meta['failures'].append(type(error).__name__)
    finally:
        def cleanup(name, operation):
            try:
                operation()
            except Exception as error:
                meta['failures'].append('cleanup ' + name + ': ' + type(error).__name__)

        def stop_service():
            if service.process.poll() is None:
                service.process.terminate()
                try:
                    service.process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    service.process.kill()
                    service.process.wait(timeout=10)

        cleanup('client processes', lambda: p4.end_all(l.user))
        if child is not None:
            cleanup('client wait', lambda: child.wait(timeout=30))
        if recorder is not None:
            cleanup('proxy', recorder.stop)
        if service is not None:
            cleanup('service process', stop_service)
            cleanup('service stdout', lambda: service.process.stdout.close())
            cleanup('service stderr', lambda: service.stderr.close())
            cleanup('service log', lambda: shutil.copyfile(service.stderr_path, out / 'service.log'))
        if firewall:
            cleanup('firewall', lambda: p4.sudo('iptables', '-D', *rule))
        if l.work.exists():
            def capture_work():
                meta['symlinks_removed'] = p4.collect_work(l, out)
            cleanup('source capture', capture_work)
        if (out / 'stream.jsonl').exists() and (out / 'proxy.jsonl').exists():
            def reconcile():
                meta['records'] = proxy.match(meta['client'], out / 'stream.jsonl', out / 'proxy.jsonl')
            cleanup('record reconciliation', reconcile)
        if meta['launched']:
            cleanup('grading', lambda: grade_collected(l, out, s, expected, token, upgrade, meta))
        meta['fixture_failure'] = not meta['launched']
        cleanup('reset', lambda: p4.reset(l))
        def inventory():
            meta['artifacts'] = p4.artifacts(out)
        cleanup('artifact inventory', inventory)
        meta['correct'] = meta['success'] = meta['launched'] and not meta['failures']
        try:
            (out / 'manifest.json').write_text(json.dumps(meta, indent=2) + '\n')
        finally:
            for sig, handler in old.items():
                signal.signal(sig, handler)
    return 0 if meta['correct'] else 1


def reserve(experiment, slot, out):
    import fcntl
    root = Path(experiment)
    root.mkdir(mode=0o700, parents=True, exist_ok=True)
    with (root / 'lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        for previous in range(1, slot):
            path = root / str(previous)
            if not path.exists() or not (Path(path.read_text()) / 'manifest.json').exists():
                raise ValueError('previous slot incomplete')
        with (root / str(slot)).open('x') as record:
            record.write(str(out))
            record.flush()
            os.fsync(record.fileno())


def grade_collected(l, out, spec, expected, token, upgrade, meta):
    candidate = out / l.work.name
    result = grader.source_precheck(l.bin / 'mo', candidate, out / 'source-tests', out / 'serve')
    (out / 'source-tests.json').write_text(result.stdout)
    if result.returncode:
        meta['failures'].append('immutable source tests failed')
    checked = subprocess.run([str(l.bin / 'mo'), 'check', str(candidate / 'main.mo'),
        '--upgrade-from', str(out / 'serve/main.mo'), '--migrate', 'migrate', '--json'],
        capture_output=True, text=True, timeout=120, check=True)
    identity = json.loads(checked.stdout)
    expected = dict(expected, source_digest=identity['candidate_source'], base_digest=identity['base_source'])
    meta['identity'] = identity
    answer = (p4.grade.last_message(out / 'stream.jsonl') or '').strip()
    pushed = grader.attestation(meta['client'], out / 'stream.jsonl', out / 'proxy.jsonl')
    verdict = grader.stopped_grade(l.bin / 'mo', out / 'data', token, upgrade,
        out / 'restart.log', expected, meta.get('live', {}), pushed,
        development=meta['development'], answer=answer)
    meta['grade'] = verdict
    meta['failures'].extend(verdict['reasons'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['setup', 'check', 'run'])
    parser.add_argument('inputs', nargs='*')
    parser.add_argument('--slot', type=int, default=1)
    parser.add_argument('--development', action='store_true')
    parser.add_argument('--limit', type=int, default=3600)
    a = parser.parse_args()
    l = layout()
    if a.action == 'setup':
        return p4.setup(l, *a.inputs)
    if a.action == 'check':
        check(l)
        freeze(a.inputs[0], l)
        return 0
    # Neutral restart reuses Phase4's private argument transport, with this entrypoint.
    p4.__file__ = __file__
    args = p4.neutral_restart(sys.argv[1:], ['run', *a.inputs], l)
    if a.inputs[:1] == ['--from']:
        raise ValueError('invalid restart')
    return collect(l, a.inputs[0], a.inputs[1], a.slot, a.development, a.limit)

if __name__ == '__main__':
    # Consume the private restart file before argparse sees its internal option.
    if sys.argv[1:3] == ['run', '--from']:
        path = Path(sys.argv[3])
        sys.argv[1:] = json.loads(path.read_text())
        path.unlink()
        p4.neutral_restart = lambda args, positional, installed: args
    sys.exit(main())
