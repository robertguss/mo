#!/usr/bin/env python3
"""Protected Phase5 collector. See runner-development.md for the private input contract."""
import argparse
import importlib.util
import json
import re
import os
from pathlib import Path
import secrets
import shutil
import signal
import subprocess
import sys
import time
import origin_identity
import origin_observer
import replay_bundle

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
ENTER = p4.ENTER.replace('os.chdir', 'env["QUEUE_UPGRADE"] = os.environ["QUEUE_UPGRADE"]; env["QUEUE_UPGRADE_BEARER"] = "Bearer " + os.environ["QUEUE_UPGRADE"]; os.chdir')


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
                         'collector_success', 'collector_timeout', 'source_tests', 'native_upgrade_header')
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


CODEX_HTTP = ['-c', 'model_provider="responses_http"', '-c',
              'model_providers.responses_http={name="OpenAI",requires_openai_auth=true,supports_websockets=false}']
CODEX_LOG = 'codex_api=trace,codex_core=debug,codex_http_client=debug'
PRIVATE_DIAGNOSTICS = re.compile(r'^\S+\s+(?:TRACE|DEBUG|INFO|WARN|ERROR)\s|codex_(?:api|core|http_client)::')


def client_environment(client, base):
    return dict(base, RUST_LOG=CODEX_LOG) if client == 'codex' else dict(base)


def public_stderr(text):
    return [line for line in text.split('\n') if line and not PRIVATE_DIAGNOSTICS.search(line)]


def command(client, binary, prompt, url, config):
    cmd = p4.client_command(client, binary, prompt, url, config)
    if client == 'codex':
        at = cmd.index('--sandbox')
        assert cmd[at + 1:at + 4] == ['workspace-write', '-c', 'sandbox_workspace_write.network_access=true']
        cmd[at + 1:at + 4] = ['danger-full-access']
        cmd[-1:-1] = ['-c', 'mcp_servers.task-queue.env_http_headers={"Mo-Upgrade-Authorization"="QUEUE_UPGRADE_BEARER"}',
                      *CODEX_HTTP]
    return cmd


def final_message(client, stream):
    events = list(proxy.base.events(stream))
    if client == 'codex':
        answer = None
        completed = False
        for event in events:
            if event.get('type') == 'turn.started':
                answer, completed = None, False
            if event.get('type') == 'item.completed':
                item = event.get('item', {})
                answer = item.get('text') if item.get('type') == 'agent_message' else None
                completed = False
            if event.get('type') == 'turn.completed':
                completed = True
            if event.get('type') in ('turn.failed', 'error'):
                completed = False
        return answer if completed and isinstance(answer, str) else None
    if client == 'claude':
        # The terminal result carries the complete final message; intermediate
        # assistant events can include narration followed by additional tool work.
        results = [e for e in events if e.get('type') == 'result']
        if len(results) == 1:
            result = results[0]
            if result.get('subtype') == 'success' and not result.get('is_error'):
                answer = result.get('result')
                return answer if isinstance(answer, str) else None
    return None


def require_no_retention_hold(installed):
    for path in (installed.work, installed.state / 'retention-hold.json'):
        try:
            path.lstat()
        except FileNotFoundError:
            continue
        raise ValueError('uncaptured work retained: recover existing work and retention hold before reset')


def reset_preserving_work(installed, out, meta):
    # Never let the legacy reset delete the only copy after failed capture.
    if meta.get('launched') and installed.work.exists() and not meta.get('source_captured'):
        origin_identity.atomic_private(installed.state / 'retention-hold.json',
                                      dict(work=str(installed.work), evidence=str(out),
                                           reason='source capture incomplete', qualified=False))
        raise RuntimeError('uncaptured work preserved; destructive reset withheld')
    p4.reset(installed)


def retain_optional(partial, role, path):
    try:
        partial.add(role, path)
        partial.facts['available'].append(role)
    except Exception as error:
        partial.facts.setdefault('unavailable', {})[role] = type(error).__name__


def collect(l, spec, out, slot, development=False, limit=3600):
    if slot not in range(1, 5) or (not development and limit != 3600):
        raise ValueError('invalid slot or time limit')
    if limit <= 0:
        raise ValueError('positive time limit required')
    if tuple(int(n) for n in os.uname().release.split('.')[:2]) < (5, 3):
        raise ValueError('capture requires Linux 5.3 or newer')
    if not development:
        raise ValueError('collection disabled: model and command-origin qualification incomplete')
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
    require_no_retention_hold(l)
    p4.reset(l)
    token, upgrade = secrets.token_urlsafe(32), secrets.token_urlsafe(32)
    for value in (token, upgrade):
        p4.append_secret(l.tokens, value)
    meta = {'collection_id': secrets.token_hex(16), 'slot': slot, 'client': SLOTS[slot-1], 'evidence_class': 'DEVELOPMENT' if development else 'FINAL',
            'success': False, 'correct': False, 'development': development, 'launched': False, 'limit_seconds': limit, 'started_at': time.time(), 'failures': []}
    service = recorder = child = None
    firewall = False
    capture = None
    bundle = None
    state_retained = False
    raw_live = {}
    trace_result = {}
    trace_run = None
    firewall_baseline = p4.sudo("iptables", "-S", "OUTPUT").stdout
    rule = None
    old = {sig: signal.signal(sig, p4.stopped) for sig in (signal.SIGTERM, signal.SIGHUP)}
    try:
        shutil.copytree(s['source'], out / 'serve')
        shutil.copytree(s['data'], out / 'start-data')
        shutil.copytree(s['data'], out / 'data')
        p4.place_read_only(s['context'], l.deployed)
        shutil.copytree(s['source'], l.work)
        p4.sudo('chown', '-R', f'{l.user}:{l.user}', l.work)
        if p4.as_session_user(l, l.bin / 'mo', 'check', l.work / 'main.mo')[0]:
            raise ValueError('session account cannot compile the supplied source')
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
                        'Mo-Upgrade-Authorization': 'Bearer ${QUEUE_UPGRADE}'}}}}))
        os.chmod(l.mcp_config, 0o444)
        prompt = Path(s['prompt']).read_text().replace('SERVICE_URL', recorder.url)
        meta['answer_rule'] = 'entire final message, stripped, solely the post-activation observed revision'
        meta['migration_requirement'] = 'migrate'
        (out / 'prompt.txt').write_text(prompt)
        cmd = command(meta['client'], l.bin / meta['client'], prompt, recorder.url, l.mcp_config)
        environment = client_environment(meta['client'], {
            'HOME': str(l.home()), 'USER': l.user, 'LOGNAME': l.user,
            'SHELL': '/bin/bash', 'PATH': f'{l.bin}:/usr/bin:/bin',
            'LANG': 'C.UTF-8', 'DISABLE_AUTOUPDATER': '1'})
        start = time.monotonic()
        if (l.bin / 'mo').stat().st_uid != 0:
            raise ValueError('frozen mo must be root owned')
        capture = origin_identity.Capture(out / 'origin-identity', str(out), l.account().pw_uid, l.bin / 'mo')
        reserve(s['experiment'], slot, out)
        meta['launched'] = True
        trace_run = origin_observer.TraceRun(out / 'origin-raw')
        trace_result = trace_run.run(
            ['/usr/bin/python3', '-I', '-c', ENTER, json.dumps(environment), str(l.work), *cmd],
            timeout=limit, env=env, session_user=l.user, cleanup=lambda: p4.end_all(l.user))
        shutil.copyfile(out / 'origin-raw/stdout', out / 'stream.jsonl')
        shutil.copyfile(out / 'origin-raw/stderr', out / 'client-stderr.txt')
        meta['kernel'] = os.uname().release
        meta['exit_status'] = trace_result['returncode']
        meta['timeout'] = trace_result['timed_out']
        meta['capture'] = trace_result
        meta['seconds'] = time.monotonic() - start
        class RecordingClient:
            def every_job(self):
                value = client.every_job()
                raw_live['jobs'] = value
                return value
            def ok(self, name):
                value = client.ok(name)
                raw_live[name] = value
                return value
        raw_live = {}
        meta['live'] = grader.live_grade(RecordingClient())
        if meta['timeout'] or meta['exit_status'] != 0:
            meta['failures'].append('timeout or unsuccessful exit')
    except BaseException as error:
        meta['failures'].append(type(error).__name__)
    finally:
        def cleanup(name, operation):
            try:
                operation()
            except Exception as error:
                meta['failures'].append('cleanup ' + name + ': ' + type(error).__name__)

        def stop_service():
            if not hasattr(service, 'process'):
                return
            if service.process.poll() is None:
                service.process.terminate()
                try:
                    service.process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    service.process.kill()
                    service.process.wait(timeout=10)

        cleanup('client processes', lambda: p4.end_all(l.user))
        meta['cleanup_zero_processes'] = False
        def verify_processes():
            found = subprocess.run(['pgrep', '-U', l.user], capture_output=True, text=True, timeout=60)
            meta['cleanup_zero_processes'] = found.returncode == 1 and not found.stdout.strip()
        cleanup('process verification', verify_processes)
        if not meta['cleanup_zero_processes']:
            cleanup('client processes retry', lambda: p4.end_all(l.user))
            cleanup('process verification retry', verify_processes)
        if child is not None:
            cleanup('client wait', lambda: child.wait(timeout=30))
        if recorder is not None:
            cleanup('proxy', recorder.stop)
        meta['service_stopped'] = service is None
        if service is not None:
            cleanup('service process', stop_service)
            meta['service_stopped'] = not hasattr(service, 'process') or service.process.poll() is not None
            cleanup('service stdout', lambda: service.process.stdout.close())
            cleanup('service stderr', lambda: service.stderr.close())
            cleanup('service log', lambda: shutil.copyfile(service.stderr_path, out / 'service.log'))
        if firewall:
            cleanup('firewall', lambda: p4.sudo('iptables', '-D', *rule))
        meta['firewall_restored'] = False
        def verify_firewall():
            meta['firewall_restored'] = p4.sudo('iptables', '-S', 'OUTPUT').stdout == firewall_baseline
        cleanup('firewall verification', verify_firewall)
        if not meta['firewall_restored']:
            cleanup('firewall verification retry', verify_firewall)
        if not meta['firewall_restored'] or not meta['cleanup_zero_processes']:
            meta['failures'].append('capture cleanup verification failed')
        meta['source_captured'] = False
        if l.work.exists() and meta['cleanup_zero_processes'] and meta['firewall_restored']:
            def capture_work():
                meta['symlinks_removed'] = p4.collect_work(l, out)
                meta['source_captured'] = True
            cleanup('source capture', capture_work)
        if (out / 'stream.jsonl').exists() and (out / 'proxy.jsonl').exists():
            def reconcile():
                nonlocal bundle, state_retained
                kwargs = {}
                if (capture is not None and trace_result.get('capture_finished') is True
                        and meta['cleanup_zero_processes'] and meta['firewall_restored']
                        and meta['service_stopped'] and meta['source_captured']):
                    trace = out / 'origin-raw/trace'
                    snapshot = out / 'origin-identity/tcp.json'
                    origin_identity.atomic_private(snapshot, dict(session=capture.session, nonce=capture.nonce,
                        trace_sha256=origin_identity.protected(trace, os.geteuid(), capture.session_uid, private=True, pin_only=True)[0]['sha256'], snapshots=recorder.recorder.snapshots))
                    capture.seal(trace, out / 'proxy.jsonl', snapshot, stdout=out / 'origin-raw/stdout',
                                 client_path=l.bin / meta['client'], stderr=out / 'origin-raw/stderr')
                    pending_bundle = replay_bundle.Bundle(out / 'replay-private', capture,
                        evaluation_endpoint=recorder.url.split('/')[2])
                    pending_bundle.add('expected', s['expected'])
                    pending_bundle.add('immutable_tests', grader.fixture.ROOT / 'services/task-queue/queue-tests.mo')
                    pending_bundle.add('spec', spec)
                    pending_bundle.add('prompt', out / 'prompt.txt')
                    for role, path in [('original', out / 'serve'), ('context', s['context']),
                                       ('candidate', out / l.work.name), ('state', out / 'data')]:
                        pending_bundle.tree(role, path)
                    pending_bundle.facts['live_observations'] = raw_live
                    pending_bundle.facts['evaluation_endpoint'] = recorder.url.split('/')[2]
                    pending_bundle.finish(dict(meta))
                    bundle = pending_bundle
                    state_retained = True
                    meta['replay_path'] = str(bundle.root)
                    kwargs = dict(capture=capture, session_id=capture.session,
                                  evaluation_endpoint=recorder.url.split('/')[2])
                meta['records'] = proxy.match(meta['client'], out / 'stream.jsonl', out / 'proxy.jsonl', **kwargs)
                if not kwargs:
                    meta['records']['match'] = False
                    meta['records']['capture_incomplete'] = True
                meta['models'] = meta['records'].pop('model_provenance')
            cleanup('record reconciliation', reconcile)
        if meta.get('models', {}).get('qualified') is not True:
            meta['failures'].append('missing or substituted model evidence')
        if meta['launched'] and bundle is None:
            def retain_partial():
                nonlocal state_retained
                if trace_run is not None and 'timeout' not in meta:
                    meta['timeout'] = getattr(trace_run, 'timed_out', None)
                partial_root = out / 'replay-private'
                if partial_root.exists():
                    partial_root = out / 'replay-partial-private'
                partial = replay_bundle.PartialBundle(partial_root, str(out), l.account().pw_uid)
                safe = (meta['cleanup_zero_processes'] and meta['firewall_restored']
                        and meta['service_stopped'] and meta['source_captured'])
                partial.facts['live_observations'] = raw_live
                for role, path in [('expected', Path(s['expected'])), ('spec', Path(spec)),
                                   ('immutable_tests', grader.fixture.ROOT / 'services/task-queue/queue-tests.mo'),
                                   ('binary', l.bin / 'mo')]:
                    if path.exists():
                        partial.add(role, path, executable=role == 'binary')
                        partial.facts['available'].append(role)
                if 'binary' in partial.files:
                    loader = origin_identity.interpreter((partial.root / 'binary').read_bytes())
                    if loader:
                        partial.facts['loader'] = origin_identity.trusted_interpreter(loader, partial.collector, partial.uid)
                        partial.add('loader', partial.facts['loader']['path'], expected=partial.facts['loader'])
                if safe:
                    for role, path in [('original', out / 'serve'), ('context', Path(s['context'])),
                                       ('candidate', out / l.work.name), ('state', out / 'data')]:
                        if path.is_dir():
                            try:
                                partial.tree(role, path)
                                partial.facts['available'].append(role)
                            except Exception as error:
                                partial.facts.setdefault('unavailable', {})[role] = type(error).__name__
                    for role, path in [('stdout', out / 'origin-raw/stdout'), ('stderr', out / 'origin-raw/stderr'),
                                       ('trace', out / 'origin-raw/trace'),
                                       ('proxy_log', out / 'proxy.jsonl'), ('prompt', out / 'prompt.txt')]:
                        if path.exists():
                            retain_optional(partial, role, path)
                partial.finish(dict(meta, timeout=meta.get('timeout')))
                state_retained = 'state' in partial.facts['available']
                meta['replay_path'] = str(partial.root)
            cleanup('partial retention', retain_partial)
        if meta['launched'] and meta['service_stopped'] and meta['source_captured'] and state_retained:
            cleanup('grading', lambda: grade_collected(l, out, s, expected, token, upgrade, meta))
        meta['fixture_failure'] = not meta['launched']
        def redact_client_artifacts():
            # The sealed raw capture remains private; redact review copies only.
            if recorder is None: return
            for name in ('stream.jsonl', 'client-stderr.txt'):
                path = out / name
                if path.exists():
                    text = path.read_text(errors='replace')
                    lines = public_stderr(text) if name == 'client-stderr.txt' else text.splitlines()
                    path.write_text('\n'.join(recorder.recorder.clean(line) for line in lines) + '\n')
        cleanup('client redaction', redact_client_artifacts)
        cleanup('reset', lambda: reset_preserving_work(l, out, meta))
        def inventory():
            meta['artifacts'] = p4.artifacts(out)
        cleanup('artifact inventory', inventory)
        dimensions = dict.fromkeys(('task_state', 'activation_identity', 'command_origin',
                                    'final_answer'), False)
        dimensions.update(meta.get('grade', {}).get('dimensions', {}))
        dimensions.update(
            source_tests=meta.get('source_tests_passed', False),
            traffic_completeness=meta.get('records', {}).get('match') is True,
            model_provenance=meta.get('models', {}).get('qualified') is True,
            publication_integrity=False)
        meta['dimensions'] = dimensions
        meta['correct'] = meta['success'] = (meta['launched'] and not meta['failures']
                                             and all(dimensions.values()))
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
    retained_tests = Path(meta.get('replay_path', out / 'replay-private')) / 'immutable_tests'
    result = grader.source_precheck(l.bin / 'mo', candidate, out / 'source-tests', out / 'serve',
        immutable_tests=retained_tests if retained_tests.exists() else None)
    (out / 'source-tests.json').write_text(result.stdout)
    meta['source_tests_passed'] = result.returncode == 0
    if result.returncode:
        meta['failures'].append('source tests timed out' if getattr(result, 'timed_out', False)
                                else 'immutable source tests failed')
    pushed = grader.attestation(meta['client'], out / 'stream.jsonl', out / 'proxy.jsonl', matched=meta.get('records'))
    if pushed is None:
        meta['failures'].append('proxy/stream push attestation failed')
    # Compile the recorded selector even when reconciliation failed; the verdict
    # still receives None and cannot credit an unattested push.
    identity = {'candidate_source': None, 'base_source': None}
    try:
        selector = grader.migration_selector(grader.accepted_report(out / 'proxy.jsonl'))
    except ValueError:
        meta['failures'].append('no accepted upgrade with a valid migration selector')
    else:
        checked = subprocess.run([str(l.bin / 'mo'), 'check', str(candidate / 'main.mo'),
            '--upgrade-from', str(out / 'serve/main.mo'), '--migrate', selector, '--json'],
            capture_output=True, text=True, timeout=120)
        (out / 'upgrade-check.json').write_text(checked.stdout)
        (out / 'upgrade-check.stderr').write_text(checked.stderr)
        if checked.returncode:
            meta['failures'].append('candidate upgrade compilation failed')
        else:
            identity = json.loads(checked.stdout)
    expected = dict(expected, source_digest=identity['candidate_source'], base_digest=identity['base_source'])
    meta['identity'] = identity
    answer = final_message(meta['client'], out / 'stream.jsonl')
    verdict = grader.stopped_grade(l.bin / 'mo', out / 'data', token, upgrade,
        out / 'restart.log', expected, meta.get('live', {}), pushed,
        development=meta['development'], answer=answer,
        identity_report=grader.accepted_report(out / 'proxy.jsonl'),
        revision_observed=meta.get('records', {}).get('revision_observed') is True)
    meta['grade'] = verdict
    meta['failures'].extend(verdict['reasons'])


def setup(installed, mo, claude, codex):
    require_no_retention_hold(installed)
    status = p4.setup(installed, mo, claude, codex)
    # The capture pins a root-owned executable with no writable mode bits.
    p4.sudo('chmod', '0555', installed.bin / 'mo')
    return status


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
        return setup(l, *a.inputs)
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
