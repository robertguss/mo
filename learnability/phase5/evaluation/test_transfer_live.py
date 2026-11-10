"""Small real-kernel development controls; no models or service credentials."""
import contextlib
import json
import os
from pathlib import Path
import platform
import shutil
import sys
import tempfile
import unittest

from origin_identity import Capture
from origin_observer import TraceRun
import proxy


class LiveTransfers(unittest.TestCase):
    def exercise(self, program, accepted):
        with contextlib.nullcontext(tempfile.mkdtemp(prefix='phase5-transfer-control-', dir=Path.home())) as tmp:
            root = Path(tmp)
            run = TraceRun(root / 'raw')
            result = run.run([sys.executable, '-c', program], timeout=10)
            (root / 'result.json').write_text(json.dumps(dict(expected=accepted, actual=result)))
            self.assertEqual(result['returncode'], 0)
            self.assertEqual(result['capture_finished'], accepted, result['integrity_errors'])
            if not accepted:
                binary = root / 'mo'
                shutil.copyfile('/bin/true', binary)
                binary.chmod(0o555)
                log = root / 'log'
                log.write_text('')
                log.chmod(0o600)
                capture = Capture(root / 'identity', 'negative-transfer', 65534, binary)
                capture.seal(run.directory / 'trace', log)
                with self.assertRaises(ValueError):
                    proxy.match('codex', run.directory / 'stdout', log,
                                capture=capture, session_id='negative-transfer')

    def test_regular_sendfile(self):
        self.exercise('import os,tempfile; a=tempfile.TemporaryFile(); b=tempfile.TemporaryFile(); '
                      'a.write(b"abc"); a.flush(); os.fstat(a.fileno()); os.fstat(b.fileno()); '
                      'assert os.sendfile(b.fileno(),a.fileno(),0,3)==3; b.seek(0); assert b.read()==b"abc"', True)

    def test_memfd_sendfile(self):
        self.exercise('import os; a=os.memfd_create("source"); b=os.memfd_create("target"); '
                      'os.write(a,b"abc"); assert os.sendfile(b,a,0,3)==3; '
                      'os.lseek(b,0,0); assert os.read(b,3)==b"abc"', True)

    def test_ordinary_python_copy(self):
        self.exercise('import tempfile,pathlib,shutil; d=tempfile.TemporaryDirectory(); '
                      'a=pathlib.Path(d.name)/"a"; b=a.with_name("b"); a.write_bytes(b"abc"); '
                      'shutil.copyfile(a,b); assert b.read_bytes()==b"abc"', True)

    def test_named_fifo(self):
        self.exercise('import os,tempfile; d=tempfile.TemporaryDirectory(); p=d.name+"/fifo"; '
                      'os.mkfifo(p); out=os.open(p,os.O_RDWR); f=tempfile.TemporaryFile(); '
                      'f.write(b"abc"); f.flush(); assert os.sendfile(out,f.fileno(),0,3)==3; '
                      'assert os.read(out,3)==b"abc"; os.close(out)', False)

    def test_socket_transfer(self):
        self.exercise('import os,socket,tempfile; a,b=socket.socketpair(); f=tempfile.TemporaryFile(); '
                      'f.write(b"abc"); f.flush(); assert os.sendfile(a.fileno(),f.fileno(),0,3)==3; '
                      'assert b.recv(3)==b"abc"', False)

    def test_shared_table_transfer_rejects(self):
        self.exercise('import os,threading; done=threading.Event(); t=threading.Thread(target=done.wait); '
                      't.start(); a=os.memfd_create("a"); b=os.memfd_create("b"); os.write(a,b"abc"); '
                      'assert os.sendfile(b,a,0,3)==3; done.set(); t.join()', False)

    def test_threaded_parent_exec_copy_passes(self):
        child = ('import pathlib,tempfile,shutil; d=tempfile.TemporaryDirectory(); '
                 'a=pathlib.Path(d.name)/"a"; b=a.with_name("b"); a.write_bytes(b"abc"); '
                 'shutil.copyfile(a,b); assert b.read_bytes()==b"abc"')
        self.exercise('import sys,threading,subprocess; done=threading.Event(); '
                      't=threading.Thread(target=done.wait); t.start(); '
                      'r=subprocess.run([sys.executable,"-c",' + repr(child) + ']); '
                      'done.set(); t.join(); assert r.returncode==0', True)

    @unittest.skipUnless(platform.machine() == 'x86_64', 'direct syscall number is x86-64 only')
    def test_direct_positioned_io_regular_file(self):
        self.exercise('import os,ctypes,tempfile; c=ctypes.CDLL(None,use_errno=True); c.syscall.restype=ctypes.c_long; '
                      'V=type("V",(ctypes.Structure,),{"_fields_":[("base",ctypes.c_void_p),("size",ctypes.c_size_t)]}); '
                      'f=tempfile.TemporaryFile(); os.fstat(f.fileno()); buf=ctypes.create_string_buffer(b"abc"); '
                      'v=V(ctypes.addressof(buf),3); '
                      'n=c.syscall(ctypes.c_long(328),ctypes.c_int(f.fileno()),ctypes.byref(v),ctypes.c_ulong(1),'
                      'ctypes.c_ulong(0),ctypes.c_ulong(0),ctypes.c_int(0)); assert n==3,(n,ctypes.get_errno()); '
                      'out=ctypes.create_string_buffer(3); v=V(ctypes.addressof(out),3); '
                      'n=c.syscall(ctypes.c_long(327),ctypes.c_int(f.fileno()),ctypes.byref(v),ctypes.c_ulong(1),'
                      'ctypes.c_ulong(0),ctypes.c_ulong(0),ctypes.c_int(0)); '
                      'assert n==3,(n,ctypes.get_errno()); assert out.raw==b"abc"', True)

    @unittest.skipUnless(platform.machine() == 'x86_64', 'direct syscall number is x86-64 only')
    def test_direct_pwritev2_pipe(self):
        self.exercise('import os,ctypes; c=ctypes.CDLL(None,use_errno=True); c.syscall.restype=ctypes.c_long; '
                      'V=type("V",(ctypes.Structure,),{"_fields_":[("base",ctypes.c_void_p),("size",ctypes.c_size_t)]}); '
                      'buf=ctypes.create_string_buffer(b"abc"); v=V(ctypes.addressof(buf),3); r,w=os.pipe(); '
                      'n=c.syscall(ctypes.c_long(328),ctypes.c_int(w),ctypes.byref(v),ctypes.c_ulong(1),'
                      'ctypes.c_ulong(-1),ctypes.c_ulong(-1),ctypes.c_int(0)); '
                      'assert n==3,(n,ctypes.get_errno()); assert os.read(r,3)==b"abc"', False)


if __name__ == '__main__':
    unittest.main()
