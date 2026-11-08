import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('runner', Path(__file__).with_name('harness.py'))
h = importlib.util.module_from_spec(spec)
spec.loader.exec_module(h)

class RunnerTests(unittest.TestCase):
    def test_local_imports_are_isolated(self):
        self.assertEqual(Path(h.grader.fixture.__file__).parent, h.HERE)
        self.assertEqual(Path(h.p4.fixture.__file__).parent.name, 'phase4')
        self.assertIs(h.grader.proxy, h.proxy)

    def test_reservation_order_and_no_reruns(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            experiment = root / 'experiment'
            out = root / 'out'
            out.mkdir()
            with self.assertRaises(ValueError):
                h.reserve(experiment, 2, out)
            h.reserve(experiment, 1, out)
            with self.assertRaises(FileExistsError):
                h.reserve(experiment, 1, out)
            with self.assertRaises(ValueError):
                h.reserve(experiment, 2, out)
            (out / 'manifest.json').write_text('{}')
            h.reserve(experiment, 2, root / 'second')

    def test_layout_is_distinct(self):
        l = h.layout()
        self.assertEqual(l.user, 'mo-phase5-session')
        self.assertNotIn('phase4', str(l.state))
        self.assertNotEqual(l.install, h.p4.Layout().install)

    def test_credentials_are_environment_references(self):
        cmd = h.command('codex', '/bin/codex', 'prompt', 'http://127.0.0.1:1/mcp', '/config')
        self.assertIn('QUEUE_UPGRADE', ' '.join(cmd))
        self.assertIn('QUEUE_TOKEN', ' '.join(cmd))
        self.assertIn('gpt-6.1-sol', cmd)
        self.assertIn('QUEUE_UPGRADE', h.ENTER)
        self.assertEqual(h.SLOTS, ['codex', 'claude', 'codex', 'claude'])

    def test_missing_freeze_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'freeze.json'
            path.write_text(json.dumps({'repository': tmp, 'repair_commit': h.BASE}))
            with self.assertRaisesRegex(ValueError, 'merged repair'):
                h.freeze(path, h.layout())

    def test_fallback_retained(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'stream'
            path.write_text('{"model":"claude-opus-4-8","fallback":true}\n')
            record = h.models(path)
            self.assertEqual(record['reported_models'], ['claude-opus-4-8'])
            self.assertTrue(record['events'][0]['fallback'])

    def test_final_limit_fixed(self):
        with self.assertRaises(ValueError):
            h.collect(h.layout(), 'absent', 'absent', 1, limit=1)

if __name__ == '__main__':
    unittest.main()
