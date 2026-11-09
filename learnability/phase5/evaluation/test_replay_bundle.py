import unittest
from test_origin_identity import IdentityTests
import replay_bundle
import origin_identity

class BundleTests(IdentityTests):
    def bundle(self):
        c = self.capture()
        c.seal(self.trace, self.log)
        b = replay_bundle.Bundle(self.root / 'bundle', c)
        for role in ('candidate', 'state', 'expected'):
            b.add(role, self.trace)
        b.finish({'launched': True})
        return b, c

    def test_jit_mapping_is_retained_without_granting_push_identity(self):
        import json
        request = 'GET / HTTP/1.1\r\n\r\n'
        self.trace.write_text(self.trace.read_text() + self.mapping() +
            '1 mprotect(0x2000, 4096, PROT_READ|PROT_EXEC) = 0\n' +
            '1 write(3<TCP:[127.0.0.1:1234->127.0.0.1:80]>, ' +
            json.dumps(request) + ', ' + str(len(request)) + ') = ' +
            str(len(request)) + '\n1 +++ exited with 0 +++\n')
        b, c = self.bundle()
        self.assertNotIn(None, b.facts['mappings'])
        parsed = c.consume(self.session)['trace']
        self.assertTrue(parsed['requests'])
        self.assertTrue(all(not r['verified_push_identity'] for r in parsed['requests']))

    def test_roundtrip_and_one_shot(self):
        b, c = self.bundle()
        result = replay_bundle.verify(b.root, self.session, self.uid)
        self.assertIn('binary', result['files'])
        c.consume(self.session)
        with self.assertRaises(origin_identity.IdentityError): c.consume(self.session)
        with self.assertRaises(origin_identity.IdentityError):
            replay_bundle.Bundle(self.root / 'late', c)

    def test_tamper(self):
        for role in ('candidate', 'state', 'expected', 'trace'):
            with self.subTest(role=role):
                b, _ = self.bundle()
                (b.root / role).write_bytes(b'changed')
                with self.assertRaises(ValueError): replay_bundle.verify(b.root, self.session, self.uid)
                import shutil
                shutil.rmtree(b.root); shutil.rmtree(self.root / 'capture')

    def test_missing_symlink_extra_session(self):
        b, _ = self.bundle()
        with self.assertRaises(ValueError): replay_bundle.verify(b.root, 'other', self.uid)
        p = b.root / 'candidate'
        p.unlink(); p.symlink_to(self.trace)
        with self.assertRaises(ValueError): replay_bundle.verify(b.root, self.session, self.uid)
        p.unlink()
        with self.assertRaises(ValueError): replay_bundle.verify(b.root, self.session, self.uid)

    def test_integration_order(self):
        from pathlib import Path
        source = Path(__file__).with_name('harness.py').read_text()
        self.assertLess(source.index('bundle.finish'), source.index("meta['records'] = proxy.match"))
        self.assertLess(source.index("('state', out / 'data')"), source.index("cleanup('grading'"))
        self.assertIn("bundle.facts['live_observations'] = raw_live", source)

    def test_extra_and_writable(self):
        b, _ = self.bundle()
        extra = b.root / 'extra'
        extra.write_text('extra')
        with self.assertRaises(ValueError): replay_bundle.verify(b.root, self.session, self.uid)
        extra.unlink()
        (b.root / 'candidate').chmod(0o666)
        with self.assertRaises(ValueError): replay_bundle.verify(b.root, self.session, self.uid)
