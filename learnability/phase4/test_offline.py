#!/usr/bin/env python3
"""The fix check (`mo replay --fix`) against the grader, on the synthetic variant's retained
`stats` failure, against a built mo.

The relation asserted is one way: a fix the grader passes is never `not_fixed` on the recorded
inputs. The converse is not asserted, because it does not hold: `verified` says the recorded
failure is gone, not that the revision computes the right answer. The variant's broken fix
removes the failure and counts a dead job as done, so the fix check calls it `verified` while
the grader fails it on the hidden tests. That cell is the expected gap, not a defect.

Run: python3 learnability/phase4/test_offline.py build/gcc/debug/mo [more mo binaries]
"""

import concurrent.futures
import json
import os
import pathlib
import secrets
import shutil
import subprocess
import sys
import tempfile
import unittest

here = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(here))
import grade  # noqa: E402

SYNTHETIC = here / 'testdata' / 'variants' / 'synthetic'
MO = None  # set per binary by main


def run(*args, timeout=900):
    return subprocess.run([str(a) for a in args], capture_output=True, text=True, check=False,
                          timeout=timeout)


class FixCheckAgainstGrader(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.TemporaryDirectory()
        root = pathlib.Path(cls.folder.name)
        token = secrets.token_hex(16)
        built = subprocess.run(
            [sys.executable, str(here / 'fixture.py'), 'build', str(SYNTHETIC), MO,
             str(root / 'built'), 'OFFLINE_TOKEN'], capture_output=True, text=True,
            timeout=600, check=False, env={**os.environ, 'OFFLINE_TOKEN': token})
        if built.returncode != 0:
            raise AssertionError(f'fixture.py build failed: {built.stderr}')
        cls.data = root / 'built' / 'data'
        history = run(MO, 'history', '--json', cls.data)
        if history.returncode != 0:
            raise AssertionError(f'mo history failed: {history.stdout} {history.stderr}')
        failures = json.loads(history.stdout.splitlines()[1])['failures']
        cls.failure = next(f['id'] for f in failures if f['handler'] == 'stats')
        # Each candidate is a whole session folder, as a session leaves it.
        cls.candidates = {}
        for name, start in (('unchanged', SYNTHETIC), ('reference', SYNTHETIC / 'hidden' / 'reference'),
                            ('broken', SYNTHETIC / 'hidden' / 'broken')):
            folder = root / name
            folder.mkdir()
            for file in grade.SESSION_FILES:
                shutil.copyfile(start / file, folder / file)
            cls.candidates[name] = folder
        # The grader runs the queue's tests, about a minute each on a debug build.
        with concurrent.futures.ThreadPoolExecutor(3) as pool:
            graded = {name: pool.submit(run, sys.executable, here / 'grade.py', 'session',
                                        SYNTHETIC, MO, folder)
                      for name, folder in cls.candidates.items()}
            cls.grades = {name: future.result() for name, future in graded.items()}
        cls.verdicts = {name: cls.fix_check(folder) for name, folder in cls.candidates.items()}

    @classmethod
    def tearDownClass(cls):
        cls.folder.cleanup()

    @classmethod
    def fix_check(cls, folder):
        result = run(MO, 'replay', '--json', SYNTHETIC / 'main.mo', '--failure', cls.failure,
                     '--data', cls.data, '--fix', folder / 'main.mo')
        records = [json.loads(line) for line in result.stdout.splitlines()]
        original = next(r for r in records if r['record'] == 'original')
        revised = next(r for r in records if r['record'] == 'revised')
        return result.returncode, original['outcome'], revised['verdict']

    def graded(self, name):
        result = self.grades[name]
        return result.returncode, json.loads(result.stdout)

    def test_the_original_failure_reproduces(self):
        result = run(MO, 'replay', SYNTHETIC / 'main.mo', '--failure', self.failure, '--data',
                     self.data)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(result.stdout.endswith('outcome: reproduced\n'), result.stdout)

    def test_the_unchanged_program_is_not_fixed_and_fails_the_grader(self):
        self.assertEqual(self.verdicts['unchanged'], (1, 'reproduced', 'not_fixed'))
        status, verdict = self.graded('unchanged')
        self.assertEqual((status, verdict['success']), (1, False), verdict)

    def test_the_reference_fix_is_verified_and_passes_the_grader(self):
        self.assertEqual(self.verdicts['reference'], (0, 'reproduced', 'verified'))
        status, verdict = self.graded('reference')
        self.assertEqual((status, verdict['success'], verdict['reasons']), (0, True, []), verdict)

    def test_the_broken_fix_is_verified_and_fails_the_grader(self):
        # The expected one-way gap: the broken fix removes the failure, so the fix check says
        # verified, and computes a wrong answer, so the hidden tests fail it.
        self.assertEqual(self.verdicts['broken'], (0, 'reproduced', 'verified'))
        status, verdict = self.graded('broken')
        self.assertEqual((status, verdict['success'], verdict['reasons']),
                         (1, False, ['HiddenTests.test.1 failed']), verdict)

    def test_a_fix_the_grader_passes_is_never_not_fixed(self):
        for name in self.candidates:
            status, verdict = self.graded(name)
            if verdict['success']:
                self.assertNotEqual(self.verdicts[name][2], 'not_fixed', name)


def main():
    binaries = sys.argv[1:]
    if not binaries:
        print(__doc__.split('\n\n')[-1], file=sys.stderr)
        return 2
    global MO
    ok = True
    for binary in binaries:
        MO = os.path.abspath(binary)
        print(f'phase 4 fix check against the grader with {binary}', file=sys.stderr)
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(FixCheckAgainstGrader)
        ok = unittest.TextTestRunner(verbosity=1).run(suite).wasSuccessful() and ok
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
