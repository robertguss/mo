import gzip
import hashlib
import io
from pathlib import Path
import tempfile
import unittest
from trace_storage import TraceLines, StorageError, compress_stream, MAX_RECORD_BYTES
from origin_trace import parse_trace
from origin_observer import process_integrity


class StorageTests(unittest.TestCase):
    def test_identical_multiple_passes_and_birth_order(self):
        raw = (b'1 execve("/bin/true", [], []) = 0\n'
               b'1 clone( <unfinished ...>\n'
               b'2 read(3</file>, "abc", 3) = 3\n'
               b'1 <... clone resumed>) = 2\n'
               b'2 +++ exited with 0 +++\n1 +++ exited with 0 +++\n')
        with tempfile.TemporaryDirectory() as tmp:
            plain, packed = Path(tmp)/'plain', Path(tmp)/'packed'
            plain.write_bytes(raw)
            receipt = compress_stream(io.BytesIO(raw), packed)
            self.assertEqual(receipt, dict(bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest()))
            self.assertEqual(gzip.decompress(packed.read_bytes()), raw)
            self.assertEqual(packed.stat().st_mode & 0o777, 0o600)
            for path in (plain, packed):
                lines = TraceLines(path)
                for _ in range(2):
                    self.assertEqual(parse_trace(lines), parse_trace(raw.decode()))
                    self.assertEqual(process_integrity(lines), process_integrity(raw.decode()))

    def test_fail_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/'trace'
            good = gzip.compress(b'1 getpid() = 1\n')
            corrupt = bytearray(good); corrupt[-8] ^= 1
            for raw in (good[:-1], bytes(corrupt), b'1 getpid() = 1', b'x'*(MAX_RECORD_BYTES+1)):
                path.write_bytes(raw)
                with self.assertRaises(StorageError):
                    parse_trace(TraceLines(path))

class LifetimeObserverRetention(unittest.TestCase):
    def test_bad_or_oversized_record_keeps_every_raw_byte(self):
        from trace_storage import RootExit
        from unittest.mock import patch
        for raw in (b'not a trace record\n' + b'x'*10000,
                    b'1 execve("' + b'x'*300 + b'", [], []) = 0\n2 tail() = 0\n'):
            with self.subTest(prefix=raw[:10]), tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp); packed=root/'trace'
                with patch('trace_storage.MAX_RECORD_BYTES', 128):
                    result=compress_stream(io.BytesIO(raw), packed, RootExit(root/'exit'))
                self.assertEqual(gzip.decompress(packed.read_bytes()),raw)
                self.assertEqual(result['bytes'],len(raw))
                self.assertEqual(result['sha256'],hashlib.sha256(raw).hexdigest())
                self.assertEqual(result['observer_error'],'StorageError')
