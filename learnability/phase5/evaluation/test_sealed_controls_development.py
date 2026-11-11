"""Two bounded opt-in development collectors, no retries or model scoring."""
import json
import os
from pathlib import Path
import shutil
import time
import unittest
from unittest.mock import patch
import protected_regrade as replay
import test_collector
from test_real_replay_development import pins

@unittest.skipUnless(os.environ.get('MO_SEALED_CONTROLS') == '1', 'development opt-in')
class SealedControls(unittest.TestCase):
    def test_two_cases(self):
        root = Path(__file__).resolve().parents[3]
        os.environ['MO_EVALUATION_BINARY'] = str(root/'build/gcc/debug/mo')
        self.assertGreater(shutil.disk_usage(root).free, 650*1024**2)
        before = set(Path.home().glob('phase5-test-private-*'))
        print('DECLARED exactly TWO DEVELOPMENT collectors: success, noop; seed 1 each; no retries', flush=True)
        start = time.monotonic()
        control = test_collector.Collector()
        result = unittest.TestResult()
        control._outcome = unittest.case._Outcome(result)
        try:
            with control._outcome.testPartExecutor(control):
                control.test_development_and_timeout(cases=('success', 'noop'))
        finally:
            control._outcome = None
        roots = set(Path.home().glob('phase5-test-private-*')) - before
        self.assertEqual(len(roots), 1)
        private = roots.pop()
        print('ARTIFACTS', private, 'collector seconds', time.monotonic()-start, 'errors', result.errors, 'failures', result.failures, flush=True)
        for case in ('success', 'noop'):
            bundle = private/('run-'+case)/'replay-private'
            baseline = pins(bundle)
            (private/(case+'-pins.json')).write_text(json.dumps(baseline))
            m = json.loads((bundle/'manifest.json').read_text())
            self.assertFalse(Path(m['provenance']['capture']['binary']['path']).exists())
            start = time.monotonic()
            try:
                with patch.object(replay.grade.fixture, 'ROOT', private/'absent-checkout'):
                    verdict = replay.regrade(bundle, session_id=m['session'], session_uid=m['session_uid'], workspace_parent=private)
                print(case, verdict, 'seconds', time.monotonic()-start, flush=True)
                (private/(case+'-verdict.json')).write_text(json.dumps(verdict))
                self.assertEqual(verdict['evidence_class'], 'DEVELOPMENT')
                self.assertEqual(len(verdict['dimensions']), 7)
                self.assertTrue(all(type(v) is bool for v in verdict['dimensions'].values()))
                self.assertFalse(verdict['dimensions']['model_provenance'])
                if case == 'success':
                    self.assertTrue(all(v for k,v in verdict['dimensions'].items() if k != 'model_provenance'))
                else:
                    self.assertFalse(verdict['dimensions']['activation_identity'])
                    self.assertFalse(verdict['dimensions']['task_state'])
                    self.assertIn('no accepted upgrade with a valid migration selector', verdict['reasons'])
                    self.assertTrue(verdict['dimensions']['source_tests'])
                    self.assertTrue(verdict['dimensions']['traffic_completeness'])
            finally:
                self.assertEqual(pins(bundle), baseline)
                print(case, 'trace bytes', (bundle/'trace').stat().st_size, 'retained files', len(baseline), 'retained bytes', sum(p.stat().st_size for p in bundle.iterdir()), flush=True)
        self.assertTrue(result.wasSuccessful(), str(result.errors+result.failures))

if __name__ == '__main__':
    unittest.main(verbosity=2)
