"""Small real-Mo DEVELOPMENT controls; zero collectors, no scoring or builds."""
import hashlib
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

import grade
import protected_regrade as replay

ROOT = Path(__file__).resolve().parents[3]
MO = ROOT / 'build/gcc/debug/mo'


class RealSourceDevelopment(unittest.TestCase):
    def test_retained_tests_survive_checkout_absence_and_change(self):
        self.assertTrue(MO.is_file())
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as tmp:
            root = Path(tmp)
            candidate = root / 'candidate'
            shutil.copytree(ROOT / 'services/task-queue', candidate)
            retained = root / 'immutable_tests'
            retained.write_bytes((candidate / 'queue-tests.mo').read_bytes())
            before = (retained.stat(), hashlib.sha256(retained.read_bytes()).hexdigest())
            checkout = root / 'checkout'
            for changed in (False, True):
                with self.subTest(changed=changed):
                    if changed:
                        fake = checkout / 'services/task-queue/queue-tests.mo'
                        fake.parent.mkdir(parents=True)
                        fake.write_text('invalid checkout fixture')
                    with patch.object(grade.fixture, 'ROOT', checkout):
                        result = grade.source_precheck(MO, candidate, root / str(changed),
                                                       immutable_tests=retained)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual((root / str(changed) / 'queue-tests.mo').read_bytes(), retained.read_bytes())
            after = (retained.stat(), hashlib.sha256(retained.read_bytes()).hexdigest())
            for field in ('st_dev', 'st_ino', 'st_mode', 'st_uid', 'st_gid', 'st_size', 'st_mtime_ns', 'st_ctime_ns'):
                self.assertEqual(getattr(before[0], field), getattr(after[0], field))
            self.assertEqual(before[1], after[1])

    def test_real_compilation_failure_and_absent_report(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as tmp:
            root = Path(tmp)
            for name in ('candidate', 'original'):
                (root / name).mkdir()
                (root / name / 'main.mo').write_text('invalid source')
            for report, reason in ((None, 'no accepted upgrade with a valid migration selector'),
                                   ({'report': {'migration': 'migrate'}}, 'candidate upgrade compilation failed')):
                with self.subTest(report=report):
                    identity, reasons = replay.compile_identity(MO, root, report)
                    self.assertEqual(identity, dict(source_digest=None, base_digest=None))
                    self.assertEqual(reasons, [reason])


if __name__ == '__main__':
    unittest.main(verbosity=2)
