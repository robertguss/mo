import os
from pathlib import Path
import tempfile
import unittest
import origin_identity as identity

class IdentityTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=Path.home())
        self.root = Path(self.tmp.name)
        self.binary = self.root / 'mo'
        self.binary.write_bytes(Path('/bin/true').read_bytes())
        self.binary.chmod(0o555)
        self.session = 'session-one'
        self.uid = 65534 if os.getuid() != 65534 else 65533
        self.trace = self.root / 'trace'
        self.log = self.root / 'log'
        self.trace.write_text('1 execve("' + str(self.binary) + '", ["mo", "push"], 0x0) = 0\n')
        self.log.write_text('')
        self.trace.chmod(0o600)
        self.log.chmod(0o600)
    def tearDown(self):
        self.tmp.cleanup()
    def capture(self):
        return identity.Capture(self.root / 'capture', self.session, self.uid, self.binary)
    def mapping(self):
        return '1 mmap(NULL, 4096, PROT_READ|PROT_EXEC, MAP_PRIVATE, 3<' + str(self.binary) + '>, 0) = 0x1000\n'
    def test_same_uid_refused(self):
        with self.assertRaises(identity.IdentityError):
            identity.Capture(self.root / 'capture', self.session, os.getuid(), self.binary)
    def test_mutable_binary_refused(self):
        self.binary.chmod(0o755)
        with self.assertRaises(identity.IdentityError): self.capture()
    def test_symlink_refused(self):
        other = self.root / 'other'; self.binary.rename(other); self.binary.symlink_to(other)
        with self.assertRaises(identity.IdentityError): self.capture()
    def test_replacement_refused(self):
        c = self.capture(); self.binary.unlink(); self.binary.write_bytes(b'fake'); self.binary.chmod(0o555)
        with self.assertRaises(identity.IdentityError): c.seal(self.trace, self.log)
    def test_tampered_trace(self):
        c = self.capture(); c.seal(self.trace, self.log); self.trace.write_text('fake')
        with self.assertRaises(identity.IdentityError): c.consume(self.session)
    def test_tampered_log(self):
        c = self.capture(); c.seal(self.trace, self.log); self.log.write_text('fake')
        with self.assertRaises(identity.IdentityError): c.consume(self.session)
    def test_wrong_session(self):
        c = self.capture(); c.seal(self.trace, self.log)
        with self.assertRaises(identity.IdentityError): c.consume('wrong')
    def test_replay(self):
        c = self.capture(); c.seal(self.trace, self.log); c.consume(self.session)
        with self.assertRaises(identity.IdentityError): c.consume(self.session)
    def test_claim_not_accepted(self):
        c = self.capture()
        with self.assertRaises(identity.IdentityError): c.consume(self.session)
    def test_identity_credit_uses_actual_exec(self):
        import json
        for arg, expected in [('push', True), ('mo push', False)]:
            with self.subTest(arg=arg):
                directory = self.root / ('capture-' + str(expected))
                c = identity.Capture(directory, self.session, self.uid, self.binary)
                request = 'GET / HTTP/1.1\r\nHost: localhost\r\n\r\n'
                self.trace.write_text('1 execve(' + json.dumps(str(self.binary)) +
                    ', ["mo", ' + json.dumps(arg) + '], []) = 0\n' + self.mapping() +
                    '1 write(3<TCP:[127.0.0.1:1234->127.0.0.1:80]>, ' +
                    json.dumps(request) + ', ' + str(len(request)) + ') = ' + str(len(request)) + '\n1 +++ exited with 0 +++\n')
                c.seal(self.trace, self.log)
                result = c.consume(self.session)
                self.assertEqual(result['trace']['requests'][0]['verified_push_identity'], expected)
                self.assertFalse(result['qualified'])

    def test_loader_injected_or_unknown_execution_cannot_receive_push_credit(self):
        import json
        contexts = {
            'preload': '["LD_PRELOAD=/session/inject.so"]',
            'audit': '["LD_AUDIT=/session/audit.so"]',
            'library_path': '["LD_LIBRARY_PATH=/session/lib"]',
            'tunables': '["GLIBC_TUNABLES=glibc.cpu.hwcaps=-AVX2"]',
            'gconv': '["GCONV_PATH=/session/lib"]',
            'unknown': '0x7fff /* 12 vars */',
            'truncated': '["PATH=/usr/bin", ...]',
        }
        request = 'GET / HTTP/1.1\r\nHost: localhost\r\n\r\n'
        for name, envp in contexts.items():
            with self.subTest(context=name):
                c = identity.Capture(self.root / ('capture-' + name),
                                     self.session, self.uid, self.binary)
                self.trace.write_text(
                    '1 execve(' + json.dumps(str(self.binary)) +
                    ', ["mo", "push"], ' + envp + ') = 0\n' + self.mapping() +
                    '1 write(3<TCP:[127.0.0.1:1234->127.0.0.1:80]>, ' +
                    json.dumps(request) + ', ' + str(len(request)) + ') = ' +
                    str(len(request)) + '\n1 +++ exited with 0 +++\n')
                c.seal(self.trace, self.log)
                result = c.consume(self.session)
                self.assertFalse(result['trace']['requests'][0]['verified_push_identity'])

    def test_untrusted_executable_mapping_cannot_receive_credit(self):
        import json
        for name, mapping in [('missing', ''), ('anonymous',
                '1 mmap(NULL, 4096, PROT_READ|PROT_EXEC, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) = 0x1000\n'),
                ('writable', '1 mmap(NULL, 4096, PROT_WRITE|PROT_EXEC, MAP_PRIVATE, 3<' + str(self.binary) + '>, 0) = 0x1000\n')]:
            with self.subTest(context=name):
                capture = identity.Capture(self.root / name, self.session, self.uid, self.binary)
                request='GET / HTTP/1.1\r\n\r\n'
                self.trace.write_text('1 execve(' + json.dumps(str(self.binary)) + ', ["mo", "push"], []) = 0\n' + mapping +
                    '1 write(3<TCP:[127.0.0.1:1234->127.0.0.1:80]>, ' + json.dumps(request) + ', ' + str(len(request)) + ') = ' + str(len(request)) + '\n1 +++ exited with 0 +++\n')
                capture.seal(self.trace,self.log)
                self.assertFalse(capture.consume(self.session)['trace']['requests'][0]['verified_push_identity'])

    def test_actual_argv_and_generation(self):
        import origin_trace
        text = self.trace.read_text() + '1 execve("/usr/bin/echo", ["echo", "mo push"], 0x0) = 0\n'
        parsed = origin_trace.parse_trace(text, session_id=self.session)
        self.assertEqual([e['generation'] for e in parsed['exec_events']], [1, 2])
        self.assertEqual(parsed['exec_events'][0]['exec_argv'], ['mo', 'push'])
        self.assertEqual(parsed['exec_events'][1]['exec_argv'], ['echo', 'mo push'])

if __name__ == '__main__': unittest.main()
