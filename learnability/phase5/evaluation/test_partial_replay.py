import unittest
import replay_bundle
import protected_regrade
import origin_identity
from test_origin_identity import IdentityTests


class PartialTests(IdentityTests):
    def partial(self):
        b = replay_bundle.PartialBundle(self.root / 'partial', self.session, self.uid)
        b.finish(dict(launched=True, development=True, slot=1, client='codex',
                      timeout=True, cleanup_zero_processes=False, firewall_restored=False))
        return b

    def test_unknown_timeout_is_not_recorded_as_false(self):
        b = replay_bundle.PartialBundle(self.root / 'partial', self.session, self.uid)
        b.finish(dict(launched=True, development=True, slot=1, client='codex',
                      timeout=None, cleanup_zero_processes=False, firewall_restored=False))
        m = replay_bundle.verify(b.root, self.session, self.uid)
        self.assertIsNone(m['launch']['timeout'])

    def test_absent_is_miss_and_class_is_protected(self):
        b = self.partial()
        (self.root / 'manifest.json').write_text(
            '{"development":false,"evidence_class":"FINAL","timeout":false,"correct":true}')
        result = protected_regrade.regrade(b.root, session_id=self.session,
            session_uid=self.uid, workspace_parent=self.root)
        self.assertEqual(result['evidence_class'], 'DEVELOPMENT')
        self.assertTrue(all(value is False for value in result['dimensions'].values()))
        self.assertTrue(result['reasons'])

    def test_corrupted_partial_rejected(self):
        b = replay_bundle.PartialBundle(self.root / 'partial', self.session, self.uid)
        b.add('expected', self.trace)
        b.finish(dict(launched=True, development=True, slot=1, client='codex',
                      timeout=True, cleanup_zero_processes=True, firewall_restored=True))
        (b.root / 'expected').write_text('corruption')
        with self.assertRaises(ValueError):
            protected_regrade.regrade(b.root, session_id=self.session,
                session_uid=self.uid, workspace_parent=self.root)

    def test_missing_record_rejected(self):
        b = self.partial()
        (b.root / 'manifest.json').unlink()
        with self.assertRaises((ValueError, OSError)):
            protected_regrade.regrade(b.root, session_id=self.session,
                session_uid=self.uid, workspace_parent=self.root)

    def test_malformed_partial_rejected(self):
        import json
        b = self.partial()
        path = b.root / 'manifest.json'
        m = json.loads(path.read_text())
        for key in ('cleanup_zero_processes', 'timeout'):
            broken = json.loads(json.dumps(m))
            del broken['launch'][key]
            path.unlink()
            origin_identity.atomic_private(path, broken)
            with self.subTest(key=key), self.assertRaises(ValueError):
                replay_bundle.verify(b.root, self.session, self.uid)

    def test_source_checks_use_retained_inputs(self):
        import shutil
        from pathlib import Path
        for executable, expected in (('/bin/true', True), ('/bin/false', False)):
            with self.subTest(executable=executable):
                root = self.root / ('pass' if expected else 'fail')
                b = replay_bundle.PartialBundle(root, self.session, self.uid)
                binary = self.root / ('binary-pass' if expected else 'binary-fail')
                binary.write_bytes(Path(executable).read_bytes())
                binary.chmod(0o555)
                b.add('binary', binary, executable=True)
                loader = origin_identity.interpreter((root / 'binary').read_bytes())
                b.facts['loader'] = origin_identity.trusted_interpreter(loader, b.collector, b.uid)
                b.add('loader', b.facts['loader']['path'], expected=b.facts['loader'])
                source = self.root / ('source-pass' if expected else 'source-fail')
                source.mkdir()
                (source / 'job-queue.mo').write_text('module JobQueue\nexpose Queue,\nstruct Queue\nend\n')
                b.tree('original', source); b.tree('candidate', source)
                b.tree('state', source)
                expected_file = source / 'expected.json'
                expected_file.write_text('{}')
                b.add('expected', expected_file)
                b.facts['live_observations'] = {'jobs': [], 'mo-state': {}}
                b.add('immutable_tests', self.trace)
                b.facts['available'] = ['binary', 'original', 'candidate', 'immutable_tests', 'state', 'expected']
                b.finish(dict(launched=True, development=True, slot=1, client='codex',
                              timeout=True, cleanup_zero_processes=True, firewall_restored=True))
                shutil.rmtree(source)
                from unittest.mock import patch
                with patch.object(protected_regrade.grade, 'stopped_grade', return_value={
                        'dimensions': {'task_state': expected, 'activation_identity': False},
                        'reasons': []}) as stopped:
                    verdict = protected_regrade.regrade(root, session_id=self.session,
                        session_uid=self.uid, workspace_parent=self.root)
                stopped.assert_called_once()  # Missing revision is observable, not absent state.
                self.assertIs(verdict['dimensions']['task_state'], expected)
                self.assertIs(verdict['dimensions']['source_tests'], expected)
                self.assertFalse(verdict['dimensions']['command_origin'])
