"""Lossless private trace streams. No publication or qualification authority."""
import gzip
import hashlib
import os
import json
import re
from contextlib import contextmanager

MAX_RECORD_BYTES = 32 * 1024 * 1024
CHUNK_BYTES = 64 * 1024


class StorageError(ValueError):
    pass


@contextmanager
def decoded(path):
    with open(path, 'rb') as raw:
        compressed = raw.read(2) == b'\x1f\x8b'
        raw.seek(0)
        if compressed:
            with gzip.GzipFile(fileobj=raw, mode='rb') as stream:
                yield stream
        else:
            yield raw


class TraceLines:
    """Each iteration opens a fresh stream and validates through gzip EOF."""
    def __init__(self, path):
        self.path = path

    def splitlines(self):
        try:
            with decoded(self.path) as stream:
                while True:
                    line = stream.readline(MAX_RECORD_BYTES + 1)
                    if not line:
                        break
                    if len(line) > MAX_RECORD_BYTES:
                        raise StorageError('oversized trace record')
                    if not line.endswith(b'\n'):
                        raise StorageError('truncated trace record')
                    yield line[:-1].decode('utf-8', errors='strict')
        except (OSError, EOFError, UnicodeError) as error:
            raise StorageError('invalid trace storage') from error

    def endswith(self, suffix):
        if suffix != '\n':
            raise ValueError('unsupported trace suffix')
        # Full iteration is required: the final gzip checksum must be verified.
        seen = False
        for _ in self.splitlines():
            seen = True
        return seen


class RootExit:
    """Read only tracer records, reporting the original tracee's terminal status."""
    def __init__(self, destination):
        self.destination = destination
        self.root = None
        self.seen = False

    def __call__(self, record):
        match = re.fullmatch(rb'(\d+)\s+(.*)\n', record)
        if not match:
            raise StorageError('invalid process record')
        pid, call = int(match[1]), match[2]
        if self.root is None:
            if not call.startswith(b'execve('):
                raise StorageError('missing initial exec')
            self.root = pid
        terminal = re.fullmatch(rb'\+\+\+ exited with (\d+) \+\+\+', call)
        if pid == self.root and terminal:
            if self.seen:
                raise StorageError('duplicate root exit')
            self.seen = True
            fd = os.open(self.destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, 'w') as out:
                json.dump({'pid': pid, 'returncode': int(terminal[1])}, out)


def compress_stream(source, destination, on_record=None):
    """Create final storage once; hash original bytes while preserving every byte."""
    digest = hashlib.sha256()
    observe = on_record
    observer_error = None
    count = 0
    fd = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'wb') as raw:
        with gzip.GzipFile(filename='', fileobj=raw, mode='wb', mtime=0) as out:
            pending = bytearray()
            read = getattr(source, 'read1', source.read)
            while chunk := read(CHUNK_BYTES):
                digest.update(chunk)
                count += len(chunk)
                out.write(chunk)
                if observe is not None:
                    pending.extend(chunk)
                    try:
                        while (end := pending.find(b'\n')) >= 0:
                            if end + 1 > MAX_RECORD_BYTES:
                                raise StorageError('oversized trace record')
                            observe(bytes(pending[:end+1]))
                            del pending[:end+1]
                        if len(pending) > MAX_RECORD_BYTES:
                            raise StorageError('oversized trace record')
                    except Exception as error:
                        # Keep every raw byte even when lifetime decoding fails.
                        observer_error = type(error).__name__
                        observe = None
                        pending.clear()
        raw.flush()
        os.fsync(raw.fileno())
    result = dict(sha256=digest.hexdigest(), bytes=count)
    if observer_error is not None:
        result['observer_error'] = observer_error
    return result
