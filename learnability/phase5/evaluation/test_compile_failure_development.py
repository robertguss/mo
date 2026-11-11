#!/usr/bin/env python3
"""Exactly one opt-in sealed DEVELOPMENT compilation failure, no retries."""
import json
import os
from pathlib import Path
import shutil
import time
import unittest
from unittest.mock import patch
import protected_regrade as replay
import publish
import test_collector
from test_real_replay_development import pins

@unittest.skipUnless(os.environ.get('MO_COMPILE_FAILURE') == '1', 'development opt-in')
class CompileFailure(unittest.TestCase):
    def test_sealed_failure(self):
        os.umask(0o077)
        root = Path(__file__).resolve().parents[3]
        os.environ['MO_EVALUATION_BINARY'] = str(root/'build/gcc/debug/mo')
        self.assertGreater(shutil.disk_usage(root).free, 350*1024**2)
        before = set(Path.home().glob('phase5-test-private-*'))
        print('DECLARED exactly ONE DEVELOPMENT compile-failure collector; no retries', flush=True)
        start = time.monotonic()
        control = test_collector.Collector()
        result = unittest.TestResult()
        control._outcome = unittest.case._Outcome(result)
        try:
            with control._outcome.testPartExecutor(control):
                control.test_development_and_timeout(cases=('compile-failure',))
        finally:
            control._outcome = None
        roots = set(Path.home().glob('phase5-test-private-*')) - before
        self.assertEqual(len(roots), 1)
        private = roots.pop()
        receipt = dict(private=str(private), collector_seconds=time.monotonic()-start,
                       collector_errors=len(result.errors), collector_failures=len(result.failures))
        bundle = private/'run-compile-failure/replay-private'
        baseline = pins(bundle)
        (private/'compile-failure-pins.json').write_text(json.dumps(baseline))
        m = json.loads((bundle/'manifest.json').read_text())
        self.assertIn('immutable_tests', m['files'])
        self.assertFalse(Path(m['provenance']['capture']['binary']['path']).exists())
        start = time.monotonic()
        try:
            with patch.object(replay.grade.fixture, 'ROOT', private/'absent-checkout'):
                verdict = replay.regrade(bundle, session_id=m['session'], session_uid=m['session_uid'], workspace_parent=private)
            receipt.update(replay_seconds=time.monotonic()-start, verdict=verdict,
                           bundle_bytes=sum(p.stat().st_size for p in bundle.iterdir()), files=len(baseline))
            (private/'compile-failure-verdict.json').write_text(json.dumps(receipt, indent=2))
            self.assertEqual(verdict['evidence_class'], 'DEVELOPMENT')
            expected = {k: k in ('task_state', 'command_origin', 'traffic_completeness', 'final_answer') for k in publish.DIMENSIONS}
            self.assertEqual(verdict['dimensions'], expected)
            self.assertTrue(all(type(v) is bool for v in verdict['dimensions'].values()))
            self.assertFalse(verdict['correct'])
            self.assertIn('candidate upgrade compilation failed', verdict['reasons'])
        finally:
            receipt['pins_unchanged'] = pins(bundle) == baseline
            (private/'compile-failure-receipt.json').write_text(json.dumps(receipt, indent=2))
            print(json.dumps(receipt), flush=True)
            self.assertEqual(pins(bundle), baseline)
        self.assertTrue(result.wasSuccessful(), 'collector assertion errors/failures retained by TestResult')

if __name__ == '__main__':
    unittest.main(verbosity=2)
