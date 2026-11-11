"""Collector-owned file pins and private writes; the collector runs no tracer."""
import hashlib
import json
import os
from pathlib import Path
import secrets
import stat
import struct


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
    tmp = path.with_name('.' + secrets.token_hex(16))
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(fd, 'w') as out:
            json.dump(value, out); out.flush(); os.fsync(out.fileno())
        os.link(tmp, path)
    finally:
        tmp.unlink(missing_ok=True)


def interpreter(data):
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
    path = Path(path)
    if not path.is_absolute(): reject()
    for item in [*reversed(path.parents), path]:
        s = item.lstat()
        if s.st_uid not in (0, collector) or s.st_uid == session: reject()
        if not stat.S_ISLNK(s.st_mode) and s.st_mode & 0o022: reject()
    return protected(path.resolve(strict=True), collector, session)[0]
