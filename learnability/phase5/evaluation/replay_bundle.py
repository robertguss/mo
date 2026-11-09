"""Collector-owned retention only. Never a grading or publication qualification."""
import shutil
from trace_storage import TraceLines
import json
import os
from pathlib import Path
import stat
import origin_identity as identity

# Real Mo capture observed 145,526,721 bytes in 3.113 seconds.
# Bound a full 3600-second session at that rate with approximately 60% headroom.
MAX_BYTES = 256 * 1024 * 1024 * 1024
MAX_FILES = 4096


class Bundle:
    def __init__(self, root, capture, *, evaluation_endpoint=None):
        if type(capture) is not identity.Capture or capture.manifest is None:
            identity.reject()
        if (capture.directory / 'consumed').exists():
            identity.reject()
        self.root = Path(root)
        self.collector, self.uid = capture.collector, capture.session_uid
        self.session = capture.session
        self.files = {}
        self.total = 0
        pin, raw = identity.protected(capture.directory / 'manifest.json', self.collector, self.uid, private=True)
        if pin != capture.manifest: identity.reject()
        manifest = json.loads(raw)
        self.root.mkdir(mode=0o700)
        self.facts = dict(capture=manifest, loader=capture.loader,
                          runtime_valid=capture.runtime_valid, mappings={})
        for role in ('binary', 'trace', 'proxy_log', 'stdout', 'tcp_snapshot'):
            if role in manifest:
                self.add(role, manifest[role]['path'], expected=manifest[role], executable=role == 'binary')
        if capture.loader:
            self.add('loader', capture.loader['path'], expected=capture.loader)
        # Record actual mappings while original paths still exist; no later inference.
        snapshot = json.loads((self.root / 'tcp_snapshot').read_bytes()) if 'tcp_snapshot' in self.files else None
        parsed = identity.origin_trace.parse_trace(TraceLines(self.root / 'trace'),
            session_id=self.session, tcp_snapshots=snapshot['snapshots'] if snapshot else (),
            evaluation_endpoint=evaluation_endpoint)
        for direction in ('requests', 'responses'):
            for receipt in parsed[direction]:
                for path in receipt['sender'].get('executable_mappings', []):
                    # Anonymous/JIT code is preserved in the trace but cannot
                    # acquire a protected mapping pin or native push identity.
                    if path is not None and path not in self.facts['mappings']:
                        pin, _ = identity.protected(path, self.collector, self.uid)
                        self.facts['mappings'][path] = pin

    def add(self, role, source, *, expected=None, executable=False):
        if len(self.files) >= MAX_FILES or role in self.files or '/' in role or role.startswith('.'):
            identity.reject()
        pin, data = identity.protected(source, self.collector, self.uid, executable=executable, pin_only=True)
        if expected is not None and pin != expected: identity.reject()
        size = pin['pin'][2]
        if self.total + size > MAX_BYTES: identity.reject()
        path = self.root / role
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        try:
            with os.fdopen(fd, 'wb') as out:
                with open(source, 'rb') as stream:
                    shutil.copyfileobj(stream, out, 65536)
                out.flush(); os.fsync(out.fileno())
            current, _ = identity.protected(source, self.collector, self.uid, executable=executable, pin_only=True)
            if current != pin: identity.reject()
            retained, _ = identity.protected(path, self.collector, self.uid, private=True, pin_only=True)
            if retained['sha256'] != pin['sha256']: identity.reject()
        except BaseException:
            # This is an unfinished copy, never an original or sealed artifact.
            path.unlink()
            raise
        self.total += size
        self.files[role] = dict(original=pin, retained=retained, role=role)

    def tree(self, role, source):
        source = Path(source)
        if source.is_symlink() or not source.is_dir(): identity.reject()
        members = []
        before, total = set(self.files), self.total
        try:
            for path in sorted(source.rglob('*')):
                if path.is_symlink(): identity.reject()
                if path.is_dir(): continue
                key = role + '-' + str(len(members))
                self.add(key, path)
                members.append(dict(path=str(path.relative_to(source)), file=key))
        except BaseException:
            for key in set(self.files) - before:
                (self.root / key).unlink()
                del self.files[key]
            self.total = total
            raise
        self.facts[role] = members

    def finish(self, facts):
        identity.atomic_private(self.root / 'manifest.json', dict(version=1,
            session=self.session, session_uid=self.uid, collector=self.collector,
            files=self.files, provenance=self.facts, launch=facts, qualified=False,
            publication_excluded=True))


def verify(root, session_id, session_uid):
    """Read only a protected collector manifest; callers supply trusted session binding.

    Returns integrity-checked metadata, never a verdict. Replay must copy inputs
    to a separate private working tree and must not modify retained files.
    """
    root = Path(root)
    _, raw = identity.protected(root / 'manifest.json', os.geteuid(), session_uid, private=True)
    m = json.loads(raw)
    if (m['session'] != session_id or m['session_uid'] != session_uid or
            m['collector'] != os.geteuid() or m['qualified'] is not False): identity.reject()
    if m.get('version') == 2:
        launch = m.get('launch', {})
        if (m.get('record_type') != 'collector-partial' or launch.get('launched') is not True
                or type(launch.get('development')) is not bool
                or type(launch.get('slot')) is not int or launch['slot'] not in range(1, 5)
                or launch.get('client') not in ('codex', 'claude')
                or any(type(launch.get(k)) is not bool for k in
                       ('cleanup_zero_processes', 'firewall_restored'))
                or 'timeout' not in launch
                or (launch.get('timeout') is not None and type(launch.get('timeout')) is not bool)
                or not isinstance(m.get('provenance', {}).get('available'), list)):
            identity.reject()
        available = m['provenance']['available']
        trees = {'original', 'context', 'candidate', 'state'}
        if (len(available) != len(set(available)) or
                any(role not in m['files'] and role not in trees for role in available) or
                any(role in available and not isinstance(m['provenance'].get(role), list) for role in trees)):
            identity.reject()
        if not (launch['cleanup_zero_processes'] and launch['firewall_restored']):
            if trees.intersection(available): identity.reject()
    elif m.get('version') != 1 or 'record_type' in m:
        identity.reject()
    if set(p.name for p in root.iterdir()) != set(m['files']) | {'manifest.json'}: identity.reject()
    for name, row in m['files'].items():
        if '/' in name or name.startswith('.') or row['role'] != name: identity.reject()
        pin, _ = identity.protected(root / name, os.geteuid(), session_uid, private=True, pin_only=True)
        if pin != row['retained'] or pin['sha256'] != row['original']['sha256']: identity.reject()
    return m


class PartialBundle(Bundle):
    """Collector retention of an incomplete launch; never a Capture receipt."""
    def __init__(self, root, session, uid):
        self.root = Path(root)
        self.collector, self.uid, self.session = os.geteuid(), uid, session
        self.files, self.total = {}, 0
        self.facts = {role: [] for role in ('original', 'context', 'candidate', 'state')}
        self.facts.update(available=[], live_observations={}, loader=None)
        self.root.mkdir(mode=0o700)

    def finish(self, facts):
        identity.atomic_private(self.root / 'manifest.json', dict(version=2,
            record_type='collector-partial', session=self.session, session_uid=self.uid,
            collector=self.collector, files=self.files, provenance=self.facts,
            launch=facts, qualified=False, publication_excluded=True))
