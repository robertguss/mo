#!/usr/bin/env python3
"""Explicit opt-in real DEVELOPMENT control; retains evidence, never retries.

Run: MO_REAL_REPLAY=1 python3 learnability/phase5/evaluation/test_real_replay_development.py
Uses the existing GCC executable, never builds it or calls a model.
"""
import hashlib
import json
import os
from pathlib import Path
import resource
import pwd
import time
import unittest

import protected_regrade as replay
import publish
import test_collector


def pins(root):
    result = {}
    for path in sorted(root.iterdir()):
        st = path.stat()
        with path.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        result[path.name] = [st.st_dev, st.st_ino, st.st_mode, st.st_uid, st.st_gid,
                             st.st_size, st.st_mtime_ns, st.st_ctime_ns, digest]
    return result


@unittest.skipUnless(os.environ.get('MO_REAL_REPLAY') == '1', 'explicit development opt-in required')
class RealReplay(unittest.TestCase):
    def test_one_collector_two_replays(self):
        binary = Path(__file__).resolve().parents[3] / 'build/gcc/debug/mo'
        self.assertTrue(binary.is_file())
        os.environ['MO_EVALUATION_BINARY'] = str(binary)
        before = set(Path.home().glob('phase5-test-private-*'))
        initial_memory = Path('/proc/self/status').read_text()
        started = time.monotonic()
        print('DECLARED: exactly ONE fresh DEVELOPMENT success collector; no retries', flush=True)
        control = test_collector.Collector()
        # A TestResult retains subtest failures instead of losing them to direct invocation.
        result = unittest.TestResult()
        control._outcome = unittest.case._Outcome(result)
        try:
            with control._outcome.testPartExecutor(control):
                control.test_development_and_timeout(cases=('success',))
        finally:
            control._outcome = None
        roots = set(Path.home().glob('phase5-test-private-*')) - before
        self.assertEqual(len(roots), 1)
        private = roots.pop()
        print('ARTIFACTS:', private, flush=True)
        self.assertTrue(result.wasSuccessful(), str(result.errors + result.failures))
        session = private / 'run-success'
        bundle = session / 'replay-private'
        manifest = json.loads((bundle / 'manifest.json').read_text())
        self.assertEqual(manifest['session'], str(session))
        self.assertEqual(manifest['session_uid'], pwd.getpwnam('mo-phase5-test').pw_uid)
        self.assertFalse(Path(manifest['provenance']['capture']['binary']['path']).exists())
        baseline = pins(bundle)
        (private / 'control-original-pins.json').write_text(json.dumps(baseline, indent=2))
        receipt = dict(evidence_class='DEVELOPMENT', collector_seconds=time.monotonic()-started,
                       trace_bytes=(bundle/'trace').stat().st_size,
                       bundle_bytes=sum(p.stat().st_size for p in bundle.iterdir()), replays=[])
        public = session / 'manifest.json'
        original = public.read_bytes()
        (private / 'original-public-manifest.json').write_bytes(original)
        callback = replay.callback({session.resolve(): (manifest['session'], manifest['session_uid'])}, private)
        try:
            for incoming in (True, False):
                self.assertEqual(pins(bundle), baseline)
                replay.replay_bundle.verify(bundle, manifest['session'], manifest['session_uid'])
                forged = json.loads(original)
                forged.update(development=False, correct=incoming, launched=True, timeout=False,
                              failures=[], fixture_label='SYNTHETIC PUBLICATION FIXTURE; NEVER SCORE',
                              dimensions=dict.fromkeys((*publish.DIMENSIONS, 'publication_integrity'), True))
                public.write_text(json.dumps(forged))
                start = time.monotonic()
                protected = callback(session)
                self.assertEqual(protected['evidence_class'], 'DEVELOPMENT')
                dimensions = protected['dimensions']
                elapsed = time.monotonic()-start
                self.assertEqual(dimensions, {k: k != 'model_provenance' for k in publish.DIMENSIONS})
                row = publish.public_rows([dict(dimensions=dimensions, execution_clean=True,
                                               correct=incoming)], True)[0]
                self.assertFalse(row['correct'])
                self.assertEqual(pins(bundle), baseline)
                replay.replay_bundle.verify(bundle, manifest['session'], manifest['session_uid'])
                receipt['replays'].append(dict(incoming_correct=incoming, seconds=elapsed,
                    dimensions=dimensions, correct=row['correct'], pins_verified_before_after=True))
                (private/'repeatable-control.json').write_text(json.dumps(receipt, indent=2))
                print('REPLAY:', receipt['replays'][-1], flush=True)
        finally:
            public.write_bytes(original)
        self.assertEqual(public.read_bytes(), original)
        self.assertEqual(pins(bundle), baseline)
        receipt['peak_rss_kib'] = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        receipt['linux_memory_before'] = [line for line in initial_memory.splitlines() if line.startswith(('VmHWM:', 'VmRSS:'))]
        receipt['linux_memory_after'] = [line for line in Path('/proc/self/status').read_text().splitlines() if line.startswith(('VmHWM:', 'VmRSS:'))]
        receipt['qualification'] = False
        receipt['publication_control_scope'] = 'actual callback and public_rows; no FINAL dataset or scored counter'
        (private/'repeatable-control.json').write_text(json.dumps(receipt, indent=2))


if __name__ == '__main__':
    unittest.main(verbosity=2)
