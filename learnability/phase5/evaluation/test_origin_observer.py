"""Collector trace lifecycle controls; no model calls."""
from unittest.mock import patch
import gzip
import hashlib
import pathlib
import tempfile
import unittest
import sys
from origin_observer import TraceRun, process_integrity


class TraceTests(unittest.TestCase):
    def test_capture_never_materializes_trace(self):
        original = pathlib.Path.read_bytes
        def guarded(path):
            if path.name == 'trace':
                raise AssertionError('whole trace read')
            return original(path)
        with tempfile.TemporaryDirectory() as tmp, patch.object(pathlib.Path, 'read_bytes', guarded):
            run = TraceRun(pathlib.Path(tmp) / 'private')
            result = run.run(['/bin/true'], timeout=5)
            self.assertTrue(result['capture_finished'])
            with gzip.open(run.directory / 'trace', 'rb') as stream:
                data = stream.read()
            self.assertEqual(result['decoded_trace'], dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))
            self.assertFalse((run.directory / 'trace.fifo').exists())

    def test_launch_failure_cleans_compressor(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = TraceRun(pathlib.Path(tmp) / 'private')
            self.assertRaises(FileNotFoundError, run.run, ['/bin/true'], timeout=5, cwd='/missing-mo-directory')
            self.assertFalse((run.directory / 'trace.fifo').exists())

    def test_memory_modification_invalidates_execution_evidence(self):
        for call in ('ptrace(PTRACE_POKEDATA, 2, 0x1000, 1) = 0',
                     'process_vm_writev(2, [], 1, [], 1, 0) = 3',
                     'write(3</proc/2/mem>, "abc", 3) = 3',
                     'pwritev2(3</proc/self/mem>, [], 1, 0, 0) = 3'):
            with self.subTest(call=call):
                trace = '1 execve("/bin/true", [], []) = 0\n1 ' + call + '\n1 +++ exited with 0 +++\n'
                self.assertFalse(process_integrity(trace)['process_completeness_verified'])

    def test_private_complete_and_single_use(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = TraceRun(pathlib.Path(tmp) / 'private')
            result = run.run(['/bin/true'], timeout=5)
            self.assertEqual(result['returncode'], 0)
            self.assertTrue(result['capture_finished'])
            self.assertEqual(run.directory.stat().st_mode & 0o777, 0o700)
            self.assertRaises(RuntimeError, run.run, ['/bin/true'], timeout=5)

    def test_timeout_does_not_qualify(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = TraceRun(pathlib.Path(tmp) / 'private')
            result = run.run(['/bin/sleep', '10'], timeout=.1)
            self.assertTrue(result['timed_out'])
            self.assertFalse(result['capture_finished'])

    def test_existing_directory_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertRaises(FileExistsError, TraceRun, pathlib.Path(tmp))

    def test_kernel_pipe_transfer_is_not_complete_evidence(self):
        programs = {'sendfile': ('import os,tempfile; f=tempfile.TemporaryFile(); '
                   'f.write(b"abc"); f.flush(); r,w=os.pipe(); '
                   'assert os.sendfile(w,f.fileno(),0,3)==3; '
                   'assert os.read(r,3)==b"abc"; os.close(r); os.close(w)'),
            'splice': ('import os; a,b=os.pipe(); r,w=os.pipe(); os.write(b,b"abc"); '
                       'assert os.splice(a,w,3)==3; assert os.read(r,3)==b"abc"'),
            'tee': ('import os,ctypes; c=ctypes.CDLL(None); a,b=os.pipe(); r,w=os.pipe(); '
                    'os.write(b,b"abc"); assert c.tee(a,w,3,0)==3; assert os.read(r,3)==b"abc"'),
            'vmsplice': ('import os,ctypes; c=ctypes.CDLL(None); r,w=os.pipe(); '
                         'V=type("V",(ctypes.Structure,),{"_fields_":[("base",ctypes.c_void_p),("size",ctypes.c_size_t)]}); '
                         'buf=ctypes.create_string_buffer(b"abc"); v=V(ctypes.addressof(buf),3); '
                         'assert c.vmsplice(w,ctypes.byref(v),1,0)==3; assert os.read(r,3)==b"abc"')}
        for name, program in programs.items():
            with self.subTest(transfer=name), tempfile.TemporaryDirectory() as tmp:
                run = TraceRun(pathlib.Path(tmp) / 'private')
                result = run.run([sys.executable, '-c', program], timeout=5)
                self.assertEqual(result['returncode'], 0)
                self.assertFalse(result['capture_finished'])
                self.assertIn('unsupported kernel transfer', result['integrity_errors'])


class DiagnosticStorageTests(unittest.TestCase):
    def test_bounded_scan_preserves_line_start_semantics(self):
        import io
        from origin_observer import tracer_diagnostic
        class Bounded(io.BytesIO):
            def read(self, size=-1):
                self_test.assertGreater(size, 0)
                self_test.assertLessEqual(size, 65536)
                return super().read(size)
        self_test = self
        for data, expected in [(b'strace: failure', True),
                               (b'ordinary strace: text', False),
                               (b'x' * 2000000, False),
                               (b'x' * 65532 + b'\nstrace: failure', True),
                               (b'normal\nstrace', False)]:
            with self.subTest(size=len(data)):
                self.assertEqual(tracer_diagnostic(Bounded(data)), expected)
