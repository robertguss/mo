"""Lossless private trace streams. No publication or qualification authority."""
import gzip
import hashlib
import os
from contextlib import contextmanager

# A 1 MiB -xx argument expands to 4 MiB, plus syscall metadata.
MAX_RECORD_BYTES = 8 * 1024 * 1024
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


def compress_stream(source, destination):
    """Create final storage once; hash original bytes while preserving every byte."""
    digest = hashlib.sha256()
    count = 0
    fd = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'wb') as raw:
        with gzip.GzipFile(filename='', fileobj=raw, mode='wb', mtime=0) as out:
            while chunk := source.read(CHUNK_BYTES):
                digest.update(chunk)
                count += len(chunk)
                out.write(chunk)
        raw.flush()
        os.fsync(raw.fileno())
    return dict(sha256=digest.hexdigest(), bytes=count)
