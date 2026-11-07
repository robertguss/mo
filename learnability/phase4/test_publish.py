#!/usr/bin/env python3
"""Tests for learnability/phase4/publish.py. Run: python3 learnability/phase4/test_publish.py"""

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

here = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(here))
import publish  # noqa: E402

REASONS = [
    'E0050 unknown name `lease_all`  job-queue.mo:12',
    'QueueTests.a lapsed lease comes back failed: expected Ready',
    'HiddenTests.the secret behaviour holds failed: expected 2, got 1',
    'hidden-tests.mo does not compile against this program: E0070 a headline',
    "the answer '41' is wrong",
]


def run(*args):
    return subprocess.run([sys.executable, str(here / 'publish.py'), *args],
                          capture_output=True, text=True, check=False, timeout=60)


class Publish(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name)
        self.out = self.root / 'out'
        self.out.mkdir()
        self.manifest = {'variant': 'synthetic', 'client': 'claude', 'seconds': 12.5}
        (self.out / 'manifest.json').write_text(json.dumps(self.manifest))
        (self.out / 'grade.json').write_text(json.dumps(
            {'variant': 'synthetic', 'client': 'claude', 'success': False,
             'grader': {'success': False, 'reasons': REASONS, 'notes': ['a note']},
             'records': {'match': True}, 'failed': [],
             'coverage': {'operator_tools': ['mo-state'], 'commands': ['replay --fix']},
             'fix_check': {'failure': '1', 'original': 'reproduced', 'verdict': 'verified',
                           'violation': False, 'graded_correct': False}}))
        (self.out / 'stream.jsonl').write_text('{}\n')
        (self.out / 'proxy.jsonl').write_text('{}\n')

    def tearDown(self):
        self.temporary.cleanup()

    def test_hidden_test_reasons_are_replaced_and_the_rest_kept(self):
        dest = self.root / 'dest'
        result = run(str(self.out), str(dest))
        self.assertEqual(result.returncode, 0, result.stderr)
        grade = json.loads((dest / 'grade.json').read_text())
        self.assertEqual(grade['grader']['reasons'],
                         [REASONS[0], REASONS[1], publish.HIDDEN, publish.HIDDEN, REASONS[4]])
        self.assertEqual(grade['grader']['notes'], ['a note'])
        self.assertFalse(grade['success'])
        text = (dest / 'grade.json').read_text()
        self.assertNotIn('secret behaviour', text)
        self.assertNotIn('E0070', text)
        self.assertEqual(json.loads((dest / 'manifest.json').read_text()), self.manifest)
        self.assertEqual(grade['coverage'],
                         {'operator_tools': ['mo-state'], 'commands': ['replay --fix']})
        self.assertEqual(grade['fix_check']['verdict'], 'verified')

    def test_only_the_manifest_and_the_grade_are_copied(self):
        dest = self.root / 'dest'
        self.assertEqual(run(str(self.out), str(dest)).returncode, 0)
        self.assertEqual(sorted(p.name for p in dest.iterdir()), ['grade.json', 'manifest.json'])

    def test_an_existing_destination_is_refused_and_left_alone(self):
        dest = self.root / 'dest'
        dest.mkdir()
        (dest / 'grade.json').write_text('earlier')
        result = run(str(self.out), str(dest))
        self.assertEqual(result.returncode, 1)
        self.assertEqual((dest / 'grade.json').read_text(), 'earlier')

    def test_a_session_without_a_grade_is_refused(self):
        (self.out / 'grade.json').unlink()
        dest = self.root / 'dest'
        self.assertEqual(run(str(self.out), str(dest)).returncode, 1)
        self.assertFalse(dest.exists())


if __name__ == '__main__':
    unittest.main()
