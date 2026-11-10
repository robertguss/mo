import tempfile
from pathlib import Path
import unittest
import protected_regrade as replay
from test_replay_bundle import BundleTests


class MissingActivation(unittest.TestCase):
    def test_absent_report_returns_identity_miss_without_compiling(self):
        from unittest.mock import patch
        with patch.object(replay.subprocess, 'run') as run:
            for report in (None, {'report': {'migration': 'invalid'}}):
                identity, reasons = replay.compile_identity('/unused/mo', Path('/unused'), report)
                self.assertEqual(identity, {'source_digest': None, 'base_digest': None})
                self.assertTrue(reasons)
            run.assert_not_called()


class ReplayTests(BundleTests):
    def test_receipt_wrong_session(self):
        b, _ = self.bundle()
        with self.assertRaises(ValueError):
            replay.ReplayReceipt(b.root, 'wrong', self.uid)

    def test_missing_inputs_fail_closed(self):
        b, _ = self.bundle()
        with self.assertRaises(ValueError):
            replay.regrade(b.root, session_id=self.session, session_uid=self.uid,
                           workspace_parent=self.root)

    def test_reconstruct_real_files_and_traversal(self):
        b, _ = self.bundle()
        m = replay.replay_bundle.verify(b.root, self.session, self.uid)
        m['provenance'].update({k: [] for k in ('original', 'context', 'candidate', 'state')})
        m['provenance']['candidate'] = [{'path': '../escape', 'file': 'candidate'}]
        with tempfile.TemporaryDirectory(dir=self.root) as tmp:
            with self.assertRaises(ValueError): replay.reconstruct(b.root, m, Path(tmp))
            self.assertFalse((self.root / 'escape').exists())

    def test_dictionary_not_receipt(self):
        with self.assertRaises(ValueError):
            replay.proxy.match('codex', self.log, self.log, capture={})

    def test_retained_executable_survives_original_removal(self):
        import subprocess
        b, _ = self.bundle()
        m = replay.replay_bundle.verify(b.root, self.session, self.uid)
        self.binary.unlink()
        with tempfile.TemporaryDirectory(dir=self.root) as tmp:
            executable = replay.runtime(b.root, m, Path(tmp), self.uid)
            self.assertEqual(subprocess.run([str(executable)], timeout=10).returncode, 0)

    def test_invalid_runtime_is_rejected_before_execution(self):
        b, _ = self.bundle()
        m = replay.replay_bundle.verify(b.root, self.session, self.uid)
        m['provenance']['runtime_valid'] = False
        with tempfile.TemporaryDirectory(dir=self.root) as tmp:
            with self.assertRaises(ValueError):
                replay.runtime(b.root, m, Path(tmp), self.uid)

    def test_receipt_rejects_invalid_runtime(self):
        import json
        b, _ = self.bundle()
        path = b.root / 'manifest.json'
        m = json.loads(path.read_text())
        m['provenance']['runtime_valid'] = False
        path.write_text(json.dumps(m))
        with self.assertRaises(ValueError):
            replay.ReplayReceipt(b.root, self.session, self.uid).consume(self.session)

    def test_small_synthetic_tamper_is_the_rejection_cause(self):
        """Fresh independent fixtures; valid verification precedes each mutation."""
        import shutil
        for role in ('trace', 'binary', 'candidate', 'state', 'expected'):
            with self.subTest(role=role):
                b, _ = self.bundle()
                try:
                    replay.replay_bundle.verify(b.root, self.session, self.uid)
                    original = (b.root / role).read_bytes()
                    (b.root / role).write_bytes(original + b'tamper')
                    with self.assertRaises(replay.identity.IdentityError):
                        replay.replay_bundle.verify(b.root, self.session, self.uid)
                finally:
                    shutil.rmtree(b.root)
                    shutil.rmtree(self.root / 'capture')

    def test_receipt_rejects_wrong_endpoint_and_loader_pin(self):
        import json
        b, _ = self.bundle()
        path = b.root / 'manifest.json'
        original = json.loads(path.read_text())
        original['provenance']['evaluation_endpoint'] = '127.0.0.1:12345'
        path.write_text(json.dumps(original))
        with self.assertRaises(ValueError):
            replay.ReplayReceipt(b.root, self.session, self.uid).consume(
                self.session, evaluation_endpoint='127.0.0.1:12346')
        original['provenance']['loader'] = dict(original['files']['binary']['original'], sha256='0'*64)
        path.write_text(json.dumps(original))
        with self.assertRaises(ValueError):
            replay.ReplayReceipt(b.root, self.session, self.uid).consume(self.session)
