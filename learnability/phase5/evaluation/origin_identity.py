"""Private, single-capture identity binding; never a completeness attestation.

Collector and isolated session must have distinct UIDs. The caller must enforce
that UID boundary (no elevated capabilities, mounts, or collector credentials).
No caller-provided receipt or identity boolean is accepted. Raw argv stays private.
"""
import hashlib
import json
import os
from pathlib import Path
import secrets
import stat
import struct

from trace_storage import TraceLines
import origin_trace
import origin_observer


class IdentityError(ValueError):
    pass


def reject():
    raise IdentityError('collector identity evidence rejected')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def protected(path, collector, session, *, executable=False, private=False, pin_only=False):
    path = Path(os.path.abspath(path))
    for item in [*reversed(path.parents), path]:
        s = item.lstat()
        if stat.S_ISLNK(s.st_mode) or s.st_uid not in (0, collector): reject()
        if s.st_uid == session or s.st_mode & 0o022: reject()
        # Reject ACLs rather than attempting to model supplementary group access.
        if 'system.posix_acl_access' in os.listxattr(item): reject()
    s = path.stat()
    if not stat.S_ISREG(s.st_mode): reject()
    if executable and (s.st_mode & 0o222 or not s.st_mode & 0o111): reject()
    if private and (s.st_uid != collector or s.st_mode & 0o077): reject()
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    try:
        before = os.fstat(fd)
        hasher = hashlib.sha256()
        parts = []
        with os.fdopen(os.dup(fd), 'rb') as stream:
            while chunk := stream.read(65536):
                hasher.update(chunk)
                if not pin_only: parts.append(chunk)
        data = None if pin_only else b''.join(parts)
        after = os.fstat(fd)
        fields = lambda x: (x.st_dev, x.st_ino, x.st_size, x.st_mtime_ns, x.st_ctime_ns, x.st_mode, x.st_uid)
        if fields(before) != fields(after) or fields(after) != fields(path.stat()): reject()
        return dict(path=str(path), pin=list(fields(after)), sha256=hasher.hexdigest()), data
    finally:
        os.close(fd)


def atomic_private(path, value):
    """Publish once, atomically, without following or replacing a destination."""
    tmp = path.with_name('.' + secrets.token_hex(16))
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(fd, 'w') as out:
            json.dump(value, out); out.flush(); os.fsync(out.fileno())
        os.link(tmp, path)
    finally:
        tmp.unlink(missing_ok=True)


def interpreter(data):
    """Read the bounded ELF64 little-endian execution header, not source text."""
    if len(data) < 64 or data[:7] != b'\x7fELF\x02\x01\x01': reject()
    offset = struct.unpack_from('<Q', data, 32)[0]
    size, count = struct.unpack_from('<HH', data, 54)
    if size != 56 or not 0 < count <= 128 or offset + size * count > len(data): reject()
    found = []
    for index in range(count):
        header = offset + index * size
        if struct.unpack_from('<I', data, header)[0] != 3: continue
        start = struct.unpack_from('<Q', data, header + 8)[0]
        length = struct.unpack_from('<Q', data, header + 32)[0]
        if not 1 < length <= 4096 or start + length > len(data): reject()
        value = data[start:start + length]
        if value[-1:] != b'\0' or b'\0' in value[:-1]: reject()
        found.append(value[:-1].decode('utf-8'))
    if len(found) > 1: reject()
    return found[0] if found else None


def trusted_interpreter(path, collector, session):
    # The interpreter path is in the pinned binary. Permit system symlinks only
    # through directories the session cannot replace; mapped library paths below
    # remain literal and reject symlinks to avoid post-execution path substitution.
    path = Path(path)
    if not path.is_absolute(): reject()
    for item in [*reversed(path.parents), path]:
        s = item.lstat()
        if s.st_uid not in (0, collector) or s.st_uid == session: reject()
        if not stat.S_ISLNK(s.st_mode) and s.st_mode & 0o022: reject()
    return protected(path.resolve(strict=True), collector, session)[0]


class Capture:
    def __init__(self, directory, session_id, session_uid, binary):
        self.collector = os.geteuid()
        if session_uid in (0, self.collector) or not session_id: reject()
        self.session_uid = session_uid
        self.session = session_id
        self.directory = Path(directory).absolute()
        self.binary, binary_bytes = protected(binary, self.collector, session_uid, executable=True)
        self.runtime_valid = False
        self.loader = None
        try:
            loader = interpreter(binary_bytes)
            self.loader = trusted_interpreter(loader, self.collector, session_uid) if loader else None
            self.runtime_valid = True
        except (IdentityError, OSError, UnicodeError, struct.error):
            pass
        self.directory.mkdir(mode=0o700)
        self.nonce = secrets.token_hex(32)
        atomic_private(self.directory / 'reservation', dict(nonce=self.nonce, session=session_id))
        protected(self.directory / 'reservation', self.collector, session_uid, private=True)
        self.manifest = None

    def seal(self, trace, proxy_log, tcp_snapshot=None, stdout=None, client_path=None, stderr=None):
        if self.manifest is not None: reject()
        current, _ = protected(self.binary['path'], self.collector, self.session_uid, executable=True)
        if current != self.binary: reject()
        if self.loader is not None:
            current, _ = protected(self.loader['path'], self.collector, self.session_uid)
            if current != self.loader: reject()
        records = {}
        for name, path in [('trace', trace), ('proxy_log', proxy_log)]:
            records[name], _ = protected(path, self.collector, self.session_uid, private=True, pin_only=True)
        if tcp_snapshot is not None:
            records['tcp_snapshot'], _ = protected(tcp_snapshot, self.collector, self.session_uid, private=True)
        if stdout is not None:
            records['stdout'], _ = protected(stdout, self.collector, self.session_uid, private=True)
        if stderr is not None:
            records['stderr'], _ = protected(stderr, self.collector, self.session_uid, private=True)
        manifest = dict(client_path=str(client_path) if client_path else None, nonce=self.nonce, session=self.session, binary=self.binary, **records)
        atomic_private(self.directory / 'manifest.json', manifest)
        self.manifest, _ = protected(self.directory / 'manifest.json', self.collector, self.session_uid, private=True)

    def consume(self, session_id, *, evaluation_endpoint=None):
        if session_id != self.session or self.manifest is None: reject()
        pin, raw = protected(self.directory / 'manifest.json', self.collector, self.session_uid, private=True)
        if pin != self.manifest: reject()
        manifest = json.loads(raw)
        if manifest['session'] != self.session or manifest['nonce'] != self.nonce: reject()
        data = {}
        for name in ('binary', 'trace', 'proxy_log', *(['tcp_snapshot'] if 'tcp_snapshot' in manifest else []), *(['stdout'] if 'stdout' in manifest else []), *(['stderr'] if 'stderr' in manifest else [])):
            pin, data[name] = protected(manifest[name]['path'], self.collector, self.session_uid,
                                        executable=name == 'binary', private=name != 'binary', pin_only=name == 'trace')
            if pin != manifest[name]: reject()
        try:
            atomic_private(self.directory / 'consumed', dict(nonce=self.nonce, session=self.session))
        except FileExistsError:
            reject()
        snapshots = []
        if 'tcp_snapshot' in data:
            snapshot = json.loads(data['tcp_snapshot'])
            if snapshot['session'] != self.session or snapshot['nonce'] != self.nonce or snapshot['trace_sha256'] != manifest['trace']['sha256']: reject()
            snapshots = snapshot['snapshots']
        parsed = origin_trace.parse_trace(TraceLines(manifest['trace']['path']), session_id=self.session, tcp_snapshots=snapshots, evaluation_endpoint=evaluation_endpoint)
        integrity = origin_observer.process_integrity(TraceLines(manifest['trace']['path']))
        loader_valid = self.loader is None or protected(self.loader['path'], self.collector, self.session_uid)[0] == self.loader
        def trusted_execution(sender):
            if not self.runtime_valid or not loader_valid or not sender.get('loader_context_clean'): return False
            # Any unsafe operation anywhere in this captured process tree can
            # invalidate an otherwise clean exec (for example a parent injection).
            if integrity['process_completeness_verified'] is not True: return False
            mappings = sender.get('executable_mappings', [])
            if self.loader is not None and not mappings: return False
            try:
                for mapped in mappings:
                    if not mapped or not Path(mapped).is_absolute(): return False
                    protected(mapped, self.collector, self.session_uid)
            except (IdentityError, OSError):
                return False
            return True
        # Only the parsed successful exec associated with this sender generation
        # can acquire identity credit. This is not command-origin attestation.
        for direction in ('requests', 'responses'):
            for receipt in parsed[direction]:
                sender = receipt['sender']
                argv = sender.get('exec_argv') or []
                receipt['verified_push_identity'] = (
                    sender['exec_path'] == self.binary['path'] and len(argv) > 1 and argv[1] == 'push'
                    and trusted_execution(sender))
        return dict(integrity=integrity, trace=parsed, proxy_rows=[json.loads(line) for line in data['proxy_log'].splitlines()],
                    stdout=data.get('stdout'), stderr=data.get('stderr'), client_path=manifest.get('client_path'), qualified=False, nonce=self.nonce)
