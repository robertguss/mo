"""Private replay of collector inputs. No model or publication qualification."""
from trace_storage import TraceLines
import json
import os
from pathlib import Path, PurePosixPath
import secrets
import shutil
import subprocess
import tempfile

import grade
import harness
import origin_observer
import pinned as identity
import origin_trace
import proxy
import replay_bundle


def launch_binding(manifest):
    launch = manifest['launch']
    return dict(collection_id=launch.get('collection_id'), started_at=launch.get('started_at'),
                execution_clean=(launch.get('launched') is True and
                    launch.get('timeout') is False and launch.get('failures') == []))


class ReplayReceipt:
    """Constructed exclusively from a verified private bundle and trusted binding."""
    def __init__(self, root, session_id, session_uid):
        self.root = Path(root)
        self.manifest = replay_bundle.verify(root, session_id, session_uid)
        self.session = session_id
        self.uid = session_uid

    def consume(self, session_id, *, evaluation_endpoint=None):
        if session_id != self.session:
            identity.reject()
        m = replay_bundle.verify(self.root, self.session, self.uid)
        p = m['provenance']
        if p.get('runtime_valid') is not True: identity.reject()
        if evaluation_endpoint is not None and evaluation_endpoint != p['evaluation_endpoint']:
            identity.reject()
        if p['loader'] is not None and identity.protected(p['loader']['path'], os.geteuid(), self.uid)[0] != p['loader']:
            identity.reject()
        for pin in p['mappings'].values():
            if pin == p['capture']['binary']: continue
            if identity.protected(pin['path'], os.geteuid(), self.uid)[0] != pin:
                identity.reject()
        raw = TraceLines(self.root / 'trace')
        snapshot = json.loads((self.root / 'tcp_snapshot').read_bytes())
        if (snapshot['session'] != self.session or snapshot['nonce'] != p['capture']['nonce']
                or snapshot['trace_sha256'] != m['files']['trace']['retained']['sha256']):
            identity.reject()
        parsed = origin_trace.parse_trace(raw, session_id=self.session,
            tcp_snapshots=snapshot['snapshots'], evaluation_endpoint=p['evaluation_endpoint'],
            channels={k: p['capture'][k]['path'] for k in ('stdout', 'stderr') if k in p['capture']} or None)
        integrity = origin_observer.process_integrity(raw)
        for direction in ('requests', 'responses'):
            for receipt in parsed[direction]:
                sender = receipt['sender']
                argv = sender.get('exec_argv') or []
                mappings = sender.get('executable_mappings', [])
                valid = all(path in p['mappings'] for path in mappings)
                receipt['verified_push_identity'] = bool(
                    sender['exec_path'] == p['capture']['binary']['path']
                    and len(argv) > 1 and argv[1] == 'push'
                    and sender.get('loader_context_clean') and valid
                    and (p['loader'] is None or mappings)
                    and integrity['process_completeness_verified'] is True)
        return dict(integrity=integrity, trace=parsed,
            proxy_rows=proxy.base.entries(self.root / 'proxy_log'),
            stdout=(self.root / 'stdout').read_bytes(),
            stderr=(self.root / 'stderr').read_bytes() if 'stderr' in m['files'] else None,
            client_path=p['capture']['client_path'], qualified=False)


def reconstruct(root, manifest, work):
    """Reject ambiguous members before writing any tree."""
    plans = []
    used = set()
    for role in ('original', 'context', 'candidate', 'state'):
        seen = set()
        for row in manifest['provenance'][role]:
            name = PurePosixPath(row['path'])
            key = row['file']
            if (name.is_absolute() or not name.parts or '..' in name.parts
                    or str(name) != row['path'] or str(name) in seen
                    or key in used or key not in manifest['files']
                    or not key.startswith(role + '-')):
                identity.reject()
            seen.add(str(name)); used.add(key)
            plans.append((Path(root) / key, work / role / str(name)))
    for role in ('original', 'context', 'candidate', 'state'):
        (work / role).mkdir(mode=0o700)
    for source, target in plans:
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open('xb') as out:
            with source.open('rb') as stream:
                shutil.copyfileobj(stream, out, 65536)


def compile_identity(mo, work, report):
    missing = dict(source_digest=None, base_digest=None)
    try:
        selector = grade.migration_selector(report)
    except ValueError:
        return missing, ['no accepted upgrade with a valid migration selector']
    checked = subprocess.run([str(mo), 'check', str(work / 'candidate/main.mo'),
        '--upgrade-from', str(work / 'original/main.mo'), '--migrate', selector, '--json'],
        capture_output=True, text=True, timeout=120)
    if checked.returncode:
        return missing, ['candidate upgrade compilation failed']
    compiled = json.loads(checked.stdout)
    return dict(source_digest=compiled['candidate_source'], base_digest=compiled['base_source']), []


def regrade(root, *, session_id, session_uid, workspace_parent):
    m = replay_bundle.verify(root, session_id, session_uid)
    if m.get('record_type') != 'collector-partial':
        identity.reject()
    return partial_regrade(root, m, session_uid, workspace_parent)


def callback(bindings, workspace_parent):
    """Bindings are collector-supplied directory -> (session ID, isolated UID)."""
    def run(directory):
        session, uid = bindings[Path(directory).resolve()]
        return regrade(Path(directory) / 'replay-private', session_id=session,
            session_uid=uid, workspace_parent=workspace_parent)
    return run


def partial_regrade(root, m, uid, workspace_parent):
    """Regrade a retained session from its retained inputs alone."""
    import publish
    root = Path(root)
    parent = Path(workspace_parent).resolve()
    if parent == root.resolve() or root.resolve() in parent.parents:
        identity.reject()
    dimensions = dict.fromkeys(publish.DIMENSIONS, False)
    reasons = []
    available = set(m['provenance']['available'])
    client = m['launch']['client']
    for role in ('binary', 'expected', 'immutable_tests', 'original', 'candidate', 'state', 'stdout', 'proxy_log'):
        if role not in available:
            reasons.append(role + ': absent or unsafe to retain')
    with tempfile.TemporaryDirectory(prefix='regrade-', dir=parent) as tmp:
        work = Path(tmp)
        reconstruct(root, m, work)
        if 'binary' in available:
            loader = identity.interpreter((root / 'binary').read_bytes())
            if loader:
                pin = identity.trusted_interpreter(loader, os.geteuid(), uid)
                if pin != m['provenance']['loader'] or identity.digest((root / 'loader').read_bytes()) != pin['sha256']:
                    identity.reject()
            mo = work / 'mo'
            shutil.copyfile(root / 'binary', mo)
            mo.chmod(0o500)
            if {'candidate', 'original', 'immutable_tests'} <= available:
                try:
                    source = grade.source_precheck(mo, work / 'candidate', work / 'tests',
                        work / 'original', immutable_tests=root / 'immutable_tests')
                    dimensions['source_tests'] = source.returncode == 0
                    reasons.append('source tests return code: ' + str(source.returncode))
                except (ValueError, OSError, subprocess.SubprocessError) as error:
                    reasons.append('source tests unavailable: ' + type(error).__name__)
            raw = m['provenance']['live_observations']
            if {'state', 'expected'} <= available and all(k in raw for k in ('jobs', 'mo-state')):
                expected = json.loads((root / 'expected').read_bytes())
                expected.update(source_digest=None, base_digest=None)
                report = None
                if {'proxy_log', 'candidate', 'original'} <= available:
                    report = grade.accepted_report(root / 'proxy_log')
                    try:
                        compiled, failures = compile_identity(mo, work, report)
                        expected.update(compiled)
                        reasons.extend(failures)
                    except (ValueError, OSError, subprocess.SubprocessError, KeyError) as error:
                        reasons.append('compilation unavailable: ' + type(error).__name__)
                answer = harness.final_message(client, root / 'stdout') if 'stdout' in available else None
                class Live:
                    def every_job(self): return raw['jobs']
                    def ok(self, name): return raw[name]
                try:
                    result = grade.stopped_grade(mo, work / 'state', secrets.token_hex(32),
                        secrets.token_hex(32), work / 'restart.log', expected, grade.observe(Live()),
                        answer=answer, identity_report=report)
                    for name in ('task_state', 'activation_identity', 'final_answer'):
                        dimensions[name] = result['dimensions'][name]
                    reasons.extend(result['reasons'])
                except (ValueError, OSError, subprocess.SubprocessError, KeyError) as error:
                    reasons.append('state check unavailable: ' + type(error).__name__)
            else:
                reasons.append('state check lacks retained live observations or inputs')
    binding = launch_binding(m)
    return dict(**binding, correct=binding['execution_clean'] and all(dimensions.values()),
        dimensions=dimensions, reasons=reasons, slot=m['launch']['slot'], client=client,
        evidence_class='DEVELOPMENT' if m['launch']['development'] else 'FINAL')
