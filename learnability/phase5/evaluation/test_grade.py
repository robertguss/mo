import json
import os
import pathlib
import secrets
import subprocess
import tempfile
import unittest
import fixture
import grade


class Selector(unittest.TestCase):
    def test_declared_function_name(self):
        for selector in ('migrate', 'Main.migrate', 'JobQueue.migrate'):
            self.assertEqual(grade.migration_selector({'report': {'migration': selector}}), selector)
        for selector in (None, 'copy', 'JobQueue.copy', 'migrate.extra', 'migrate()', '..migrate'):
            with self.assertRaises(ValueError):
                grade.migration_selector({'report': {'migration': selector}})


class Outcome(unittest.TestCase):
    expected = dict(jobs=[], revision='1', stored_revision='1', next_id='1',
                    source_digest='new', base_digest='old')
    live = dict(jobs=[], stored_jobs=[], revision='1', stored_revision='1',
                next_id='1', state_complete=True)
    report = {'report': dict(status='accepted', candidate_source='new',
                             active_source='new', ordinal='2')}
    history = dict(program=dict(source='new', ordinal='2'),
                   upgrades=[dict(new_source='new', old_source='old', ordinal='2')])

    def grade(self, answer='1', live=None, history=None, report=report):
        live = self.live if live is None else live
        return grade.compare(self.expected, live, live, history or self.history,
                             answer=answer, identity_report=report)

    def test_correct_upgrade_passes_on_every_dimension(self):
        for answer in ('1', ' 1\n'):
            verdict = self.grade(answer)
            self.assertTrue(verdict['correct'], verdict)
            self.assertEqual(verdict['dimensions'], dict(task_state=True, activation_identity=True,
                                                         final_answer=True))

    def test_answer_in_prose_is_a_miss(self):
        for answer in ('The revision is 1.', 'Revision: 1', '`1`', '', None):
            verdict = self.grade(answer)
            self.assertFalse(verdict['correct'])
            self.assertEqual(verdict['dimensions'], dict(task_state=True, activation_identity=True,
                                                         final_answer=False))

    def test_answer_must_match_the_live_reading(self):
        verdict = self.grade('1', live=dict(self.live, revision='2'))
        self.assertFalse(verdict['dimensions']['final_answer'])
        self.assertTrue(self.grade('2', live=dict(self.live, revision='2'))['dimensions']['final_answer'])

    def test_no_upload_or_extra_activation_is_an_identity_miss(self):
        for history, report in ((self.history, None),
                                (dict(self.history, upgrades=self.history['upgrades'] * 2), self.report),
                                (dict(self.history, program=dict(source='wrong', ordinal='2')), self.report)):
            verdict = self.grade(history=history, report=report)
            self.assertFalse(verdict['correct'])
            self.assertFalse(verdict['dimensions']['activation_identity'])
            self.assertTrue(verdict['dimensions']['task_state'])


class RetainedSourceTests(unittest.TestCase):
    def test_source_timeout_is_a_diagnostic_result(self):
        from unittest.mock import patch
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            candidate = root / 'candidate'; candidate.mkdir()
            retained = root / 'tests'; retained.write_text('captured tests')
            with patch.object(grade.subprocess, 'run', side_effect=subprocess.TimeoutExpired('mo', 240)):
                result = grade.source_precheck('/unused/mo', candidate, root/'work', immutable_tests=retained)
            self.assertEqual(result.returncode, 124)
            self.assertTrue(result.timed_out)

    def test_uses_retained_tests_when_checkout_is_absent(self):
        from unittest.mock import patch
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            candidate = root / 'candidate'
            candidate.mkdir()
            (candidate / 'queue-tests.mo').write_text('untrusted candidate tests')
            retained = root / 'retained-tests'
            retained.write_text('immutable captured tests')
            with patch.object(fixture, 'ROOT', root / 'absent'), patch.object(grade.subprocess, 'run'):
                grade.source_precheck('/unused/mo', candidate, root / 'work',
                                      immutable_tests=retained)
            self.assertEqual((root / 'work/queue-tests.mo').read_text(), 'immutable captured tests')


class RealControls(unittest.TestCase):
    def test_real_services(self):
        mo = pathlib.Path(os.environ.get('MO_EVAL_BINARY', str(fixture.ROOT / 'build/eval-controls/debug/mo')))
        self.assertTrue(mo.is_file(), 'build isolated current binary first')
        for control in ('reference', 'qualified-reference', 'unchanged', 'initializer', 'drop-job', 'payload',
                        'duplicate-completion', 'wrong-revision', 'source-only',
                        'constant-revision', 'wrong-next-id'):
            with self.subTest(control=control), tempfile.TemporaryDirectory() as temp:
                root = pathlib.Path(temp)
                token, upgrade = secrets.token_hex(24), secrets.token_hex(24)
                expected = fixture.build(mo, root / 'prepared', token, upgrade)
                old = root / 'prepared/old'
                candidate = fixture.sources(root / 'candidate', 'reference' if control == 'source-only' else control)
                selector = 'JobQueue.migrate' if control == 'qualified-reference' else 'migrate'
                service = fixture.Service(mo, root / 'prepared/data', token, upgrade, root / 'live.log', old / 'main.mo')
                try:
                    client = fixture.Client(service.url, token)
                    self.assertTrue(grade.precheck(client, expected))
                    args = [str(mo), 'check', str(candidate / 'main.mo'), '--upgrade-from', str(old / 'main.mo'), '--json']
                    if control != 'unchanged':
                        args += ['--migrate', selector]
                    checked = subprocess.run(args, capture_output=True, text=True, timeout=120)
                    self.assertEqual(checked.returncode, 0, checked.stderr)
                    identity = json.loads(checked.stdout)
                    expected.update(source_digest=identity['candidate_source'], base_digest=identity['base_source'])
                    pushed = None
                    if control not in ('unchanged', 'source-only'):
                        result = subprocess.run([str(mo), 'push', str(candidate / 'main.mo'), '--url', service.url,
                            '--base', identity['base_source'], '--migrate', selector, '--token-env', 'QUEUE_TOKEN',
                            '--upgrade-token-env', 'QUEUE_UPGRADE', '--json'],
                            env={**os.environ, 'QUEUE_TOKEN': token, 'QUEUE_UPGRADE': upgrade},
                            capture_output=True, text=True, timeout=180)
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        pushed = {'report': json.loads(result.stdout)}
                    if control in ('reference', 'qualified-reference'):
                        self.assertEqual(grade.migration_selector(pushed), selector)
                    live = grade.live_grade(client)
                finally:
                    service.stop()
                verdict = grade.stopped_grade(mo, root / 'prepared/data', token, upgrade, root / 'restart.log',
                    expected, live, development=True, answer='1', identity_report=pushed)
                self.assertEqual(verdict['correct'], control in ('reference', 'qualified-reference'), verdict)
                print(control, verdict)
                if control == 'drop-job':
                    forged = dict(live, jobs=expected['jobs'])
                    self.assertFalse(grade.compare(expected, forged, forged,
                        grade.history(mo, root / 'prepared/data'),
                        development=True, answer='1', identity_report=pushed)['correct'])
                if control == 'reference':
                    leased = [j for j in expected['jobs'] if j['state']['case'] == 'Leased']
                    self.assertEqual(len(leased), 1)
                    self.assertEqual(leased[0]['state']['fields']['until'], '2099-01-01T00:30:00Z')
                    self.assertEqual(len(expected['preparation_upgrades']), 1)
                    wrong_prefix = dict(expected, preparation_upgrades=[])
                    self.assertFalse(grade.compare(wrong_prefix, live, live,
                        grade.history(mo, root / 'prepared/data'),
                        development=True, answer='1', identity_report=pushed)['correct'])



    def test_prepared_lease_ages_only_at_declared_deadline(self):
        import shutil
        mo = pathlib.Path(os.environ.get('MO_EVAL_BINARY', str(fixture.ROOT / 'build/eval-controls/debug/mo')))
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            token, upgrade = secrets.token_hex(24), secrets.token_hex(24)
            fixture.build(mo, root / 'prepared', token, upgrade)
            for name in ('main.mo', 'job-queue.mo'):
                self.assertEqual((root / 'prepared/old' / name).read_bytes(),
                                 (fixture.ROOT / 'services/task-queue' / name).read_bytes())
            service = fixture.Service(mo, root / 'prepared/data', token, upgrade, root / 'export.log')
            try:
                subprocess.run([str(mo), 'fetch', service.url, '--export', 'state',
                    '--out', str(root / 'state.moex'), '--token-env', 'QUEUE_TOKEN'],
                    env={**os.environ, 'QUEUE_TOKEN': token}, capture_output=True,
                    text=True, timeout=120, check=True)
            finally:
                service.stop()
            query = root / 'query'
            shutil.copytree(root / 'prepared/old', query)
            with (query / 'main.mo').open('a') as output:
                output.write('\nfn fixture_age(queue: Queue) : Bool\n'
                    '  early = Time.parse("2098-12-31T23:59:59Z") or stop "fixture"\n'
                    '  late = Time.parse("2099-01-01T00:30:00Z") or stop "fixture"\n'
                    '  jobs = JobQueue.jobs(queue)\n'
                    '  JobQueue.jobs(JobQueue.expire(queue, early)) == jobs and\n'
                    '    JobQueue.jobs(JobQueue.expire(queue, late)) != jobs\nend\n')
            checked = subprocess.run([str(mo), 'query', str(query / 'main.mo'),
                'fixture_age', '--export', str(root / 'state.moex')],
                capture_output=True, text=True, timeout=120)
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
            self.assertEqual(checked.stdout.strip(), 'true')


if __name__ == '__main__':
    unittest.main()
