#!/usr/bin/env python3
"""Tests for learnability/phase4/grade.py, on the synthetic variant in testdata/, against a
built mo.

Run: python3 learnability/phase4/test_grade.py build/gcc/debug/mo [more mo binaries]
"""

import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

here = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(here))
import grade  # noqa: E402

TESTDATA = here / 'testdata'
SYNTHETIC = TESTDATA / 'variants' / 'synthetic'
MO = None  # set per binary by main


def tool(*args):
    return subprocess.run([sys.executable, str(here / 'grade.py'), *map(str, args)],
                          capture_output=True, text=True, check=False, timeout=900)


def claude(*texts):
    """A Claude Code stream-json stream whose assistant messages are the texts, in order."""
    events = [{'type': 'system', 'subtype': 'init', 'tools': []}]
    for i, text in enumerate(texts):
        events.append({'type': 'assistant', 'message': {'id': f'msg_{i}', 'content': [
            {'type': 'text', 'text': text}]}})
        events.append({'type': 'user', 'message': {'content': [{'type': 'tool_result'}]}})
    events.append({'type': 'result', 'result': texts[-1] if texts else ''})
    return '\n'.join(json.dumps(e) for e in events) + '\n'


def codex(*texts):
    events = [{'type': 'thread.started'}]
    for text in texts:
        events.append({'type': 'item.completed', 'item': {'type': 'agent_message',
                                                          'text': text}})
    events.append({'type': 'turn.completed'})
    return '\n'.join(json.dumps(e) for e in events) + '\n'


class Answers(unittest.TestCase):
    def last(self, stream):
        with tempfile.NamedTemporaryFile('w', suffix='.jsonl', delete=False) as file:
            file.write(stream)
        try:
            return grade.extracted(grade.last_message(file.name))
        finally:
            os.unlink(file.name)

    def test_each_client_gives_the_last_messages_answer(self):
        for make in (claude, codex):
            self.assertEqual(self.last(make('ANSWER: 9', 'Counted them.\nANSWER: 15')), '15')
            self.assertEqual(self.last(make('Done.\n`ANSWER: 15`')), '15')
            self.assertEqual(self.last(make('**ANSWER:** `fail 2026-01-31T09:05:00Z`')),
                             'fail 2026-01-31T09:05:00Z')

    def test_no_answer_or_two_different_answers_give_none(self):
        for make in (claude, codex):
            self.assertIsNone(self.last(make('ANSWER: 15', 'I could not tell.')))
            self.assertIsNone(self.last(make('ANSWER: 15\nANSWER: 16')))
            self.assertEqual(self.last(make('ANSWER: 15\nANSWER: 15')), '15')

    def test_a_claude_message_in_several_events_is_one_message(self):
        stream = '\n'.join(json.dumps(e) for e in [
            {'type': 'assistant', 'message': {'id': 'm', 'content': [
                {'type': 'text', 'text': 'Answer below.'}]}},
            {'type': 'assistant', 'message': {'id': 'm', 'content': [
                {'type': 'text', 'text': 'ANSWER: 4'}]}}]) + '\n'
        self.assertEqual(self.last(stream), '4')

    def test_times_compare_to_the_second_in_utc(self):
        rule = 'killing_call'
        key = grade.normalized(rule, 'fail 2026-11-04T12:09:49Z')
        for given in ('fail 2026-11-04T12:09:49Z', 'FAIL 2026-11-04T12:09:49.731Z',
                      'fail 2026-11-04T13:09:49+01:00'):
            self.assertEqual(grade.normalized(rule, given), key, given)
        for given in ('fail 2026-11-04T12:09:50Z', 'lease 2026-11-04T12:09:49Z',
                      'fail 2026-11-04T12:09:49', 'fail', 'fail at noon'):
            self.assertNotEqual(grade.normalized(rule, given), key, given)
        self.assertEqual(grade.normalized('shared_payload_jobs', '15'), '15')
        self.assertIsNone(grade.normalized('shared_payload_jobs', 'about 15'))


class Blocks(unittest.TestCase):
    def test_blocks_are_the_tests_examples_and_promises(self):
        found = grade.blocks((SYNTHETIC / 'job-queue.mo').read_text())
        self.assertEqual([b.split('\n')[0].strip() for b in found],
                         ['examples', 'test "a job given back three times is dead"'])
        self.assertTrue(all(b.rstrip().endswith('end') for b in found))


class Grading(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.folder.name)

    def tearDown(self):
        self.folder.cleanup()

    def session(self, start, edit=None):
        """A session's folder: a copy of start, edited by edit(folder)."""
        folder = self.root / f'session-{len(list(self.root.iterdir()))}'
        folder.mkdir()
        for name in grade.SESSION_FILES:
            shutil.copyfile(start / name, folder / name)
        if edit:
            edit(folder)
        return folder

    def graded(self, folder, variant=SYNTHETIC, *extra):
        result = tool('session', variant, MO, folder, *extra)
        return result.returncode, json.loads(result.stdout) if result.stdout else result.stderr

    def test_the_reference_passes_and_the_unfixed_variant_fails_a_hidden_test(self):
        status, verdict = self.graded(self.session(SYNTHETIC / 'hidden' / 'reference'))
        self.assertEqual((status, verdict['success'], verdict['reasons']), (0, True, []))
        status, verdict = self.graded(self.session(SYNTHETIC))
        self.assertEqual((status, verdict['success']), (1, False))
        self.assertEqual(verdict['reasons'], ['HiddenTests.test.1 failed'])

    def test_a_renamed_tool_fails_on_the_tests_compile_error(self):
        def rename(folder):
            text = (folder / 'main.mo').read_text()
            (folder / 'main.mo').write_text(text.replace('fail, list, stats', 'fail, list, counts')
                                            .replace('fn stats(', 'fn counts('))
        status, verdict = self.graded(self.session(SYNTHETIC / 'hidden' / 'reference', rename))
        self.assertEqual(status, 1)
        self.assertEqual(verdict['reasons'][0],
                         "main.mo's service declaration differs from the variant's")
        self.assertTrue(any(' E0' in reason or reason.startswith('E0')
                            for reason in verdict['reasons'][1:]), verdict['reasons'])

    def test_a_changed_service_declaration_fails(self):
        def slower_timer(folder):
            text = (folder / 'main.mo').read_text()
            (folder / 'main.mo').write_text(text.replace('every 1.minute expire',
                                                         'every 7.minutes expire'))

        def added_tool(folder):
            text = (folder / 'main.mo').read_text()
            (folder / 'main.mo').write_text(
                text.replace('fail, list, stats', 'fail, list, stats, total') +
                '\n## Counts every job.\nfn total(queue: Queue, _now: Time) : Int\n'
                '  JobQueue.jobs(queue).size\nend\n')
        for edit in (slower_timer, added_tool):
            status, verdict = self.graded(self.session(SYNTHETIC / 'hidden' / 'reference', edit))
            self.assertEqual(status, 1, edit.__name__)
            self.assertEqual(verdict['reasons'],
                             ["main.mo's service declaration differs from the variant's"])

    def test_an_edited_queue_tests_file_is_noted_and_not_used(self):
        def weaken(folder):
            (folder / 'queue-tests.mo').write_text('module QueueTests\n')
        status, verdict = self.graded(self.session(SYNTHETIC / 'hidden' / 'reference', weaken))
        self.assertEqual((status, verdict['success']), (0, True))
        self.assertEqual(verdict['notes'],
                         ["the session changed queue-tests.mo; the variant's copy was used"])

    def test_job_queue_keeps_the_sessions_code_and_the_variants_blocks(self):
        def never_dead(folder):
            path = folder / 'job-queue.mo'
            path.write_text(path.read_text().replace(
                'if job.attempts >= max_attempts: Dead else: Ready', 'Ready'))
        status, verdict = self.graded(self.session(SYNTHETIC / 'hidden' / 'reference',
                                                   never_dead))
        self.assertEqual(status, 1)
        self.assertIn('JobQueue.test.1 failed', verdict['reasons'])

        def dropped_example(folder):
            path = folder / 'job-queue.mo'
            path.write_text(path.read_text().replace(
                '  examples\n    expire(empty, t0) == empty\n  end\n\n', ''))
        status, verdict = self.graded(self.session(SYNTHETIC / 'hidden' / 'reference',
                                                   dropped_example))
        self.assertEqual(status, 1)
        self.assertEqual(verdict['reasons'], [grade.BLOCKS_CHANGED])

    def test_checks_added_to_job_queue_are_kept_and_changed_ones_fail(self):
        def edit(old, replacement):
            def apply(folder):
                path = folder / 'job-queue.mo'
                text = path.read_text()
                self.assertIn(old, text)
                path.write_text(text.replace(old, replacement, 1))
            return apply
        example = '    expire(empty, t0) == empty\n'
        added = {
            'an example at the end of the examples': edit(
                example, example + '    expire(expire(empty, t0), t0) == empty\n'),
            'examples of their own': edit(
                'fn jobs(queue: Queue) : List(Job)\n',
                'fn jobs(queue: Queue) : List(Job)\n  examples\n    jobs(empty) == []\n  end\n\n'),
            'a test of its own': edit(
                'test "a job given back three times is dead"',
                'test "an empty queue has no jobs"\n  assert empty.jobs.size == 0\nend\n\n'
                'test "a job given back three times is dead"')}
        for name, change in added.items():
            status, verdict = self.graded(self.session(SYNTHETIC / 'hidden' / 'reference',
                                                       change))
            self.assertEqual((status, verdict['reasons']), (0, []), name)
        changed = {
            'a changed example': edit(example, '    expire(empty, t0 + 1.second) == empty\n'),
            'a line put inside a test': edit(
                '    assert job.attempts == attempt\n',
                '    assert job.attempts >= 0\n    assert job.attempts == attempt\n')}
        for name, change in changed.items():
            status, verdict = self.graded(self.session(SYNTHETIC / 'hidden' / 'reference',
                                                       change))
            self.assertEqual((status, verdict['reasons']), (1, [grade.BLOCKS_CHANGED]), name)

    def test_a_question_is_graded_on_its_answer_and_the_queues_tests(self):
        question = self.root / 'question'
        shutil.copytree(SYNTHETIC, question)
        shutil.rmtree(question / 'hidden')
        data = json.loads((question / 'fixture.json').read_text())
        data['key'] = {'rule': 'shared_payload_jobs'}
        (question / 'fixture.json').write_text(json.dumps(data))
        key = self.root / 'key.json'
        key.write_text(json.dumps({'answer': '2'}))
        for stream, success in ((claude('There are two.\nANSWER: 2'), True),
                                (codex('ANSWER: 3'), False), (codex('two'), False)):
            path = self.root / 'stream.jsonl'
            path.write_text(stream)
            status, verdict = self.graded(self.session(question), question, path, key)
            self.assertEqual((status == 0, verdict['success']), (success, success), stream)
        status, _ = self.graded(self.session(question), question)
        self.assertEqual(status, 2)


class FixCheck(unittest.TestCase):
    """mo replay --fix of a session's folder against the fixture's retained failure."""

    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.TemporaryDirectory()
        cls.root = pathlib.Path(cls.folder.name)
        built = subprocess.run([sys.executable, str(here / 'fixture.py'), 'build', SYNTHETIC, MO,
                                cls.root / 'built', 'BUILD_TOKEN'], capture_output=True,
                               text=True, timeout=900, env={**os.environ, 'BUILD_TOKEN': 'b' * 32})
        assert built.returncode == 0, built.stdout + built.stderr

    @classmethod
    def tearDownClass(cls):
        cls.folder.cleanup()

    def test_the_reference_fix_is_verified_and_the_unfixed_folder_is_not(self):
        data = self.root / 'built' / 'data'
        record = grade.fix_check(SYNTHETIC, MO, data, SYNTHETIC / 'hidden' / 'reference')
        self.assertEqual({k: record[k] for k in ('original', 'verdict', 'violation')},
                         {'original': 'reproduced', 'verdict': 'verified', 'violation': False})
        record = grade.fix_check(SYNTHETIC, MO, data, SYNTHETIC)
        self.assertEqual((record['original'], record['verdict'], record['violation']),
                         ('reproduced', 'not_fixed', False))

    def test_verified_beside_an_unreproduced_original_is_a_violation(self):
        fake = self.root / 'fake-mo'
        fake.write_text("#!/bin/sh\nif [ \"$1\" = history ]; then\n"
                        "echo '{\"record\":\"history\",\"failures\":"
                        "[{\"id\":\"3\",\"handler\":\"stats\"}]}'\nelse\n"
                        "echo '{\"record\":\"original\",\"outcome\":\"not_reproduced\"}'\n"
                        "echo '{\"record\":\"revised\",\"verdict\":\"verified\"}'\nfi\n")
        fake.chmod(0o755)
        record = grade.fix_check(SYNTHETIC, fake, self.root, SYNTHETIC)
        self.assertEqual(record, {'failure': '3', 'original': 'not_reproduced',
                                  'verdict': 'verified', 'violation': True})

    def test_a_question_variant_or_a_folder_without_its_failure_gets_no_fix_check(self):
        fake = self.root / 'no-failures'
        fake.write_text("#!/bin/sh\necho '{\"record\":\"history\",\"failures\":[]}'\n")
        fake.chmod(0o755)
        self.assertIsNone(grade.fix_check(SYNTHETIC, fake, self.root, SYNTHETIC))


class Check(unittest.TestCase):
    def test_the_synthetic_variant_is_sound(self):
        result = tool('--check', MO, TESTDATA)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('ok   synthetic', result.stdout)

    def test_a_stale_hash_or_a_leaking_report_is_named(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            shutil.copytree(TESTDATA / 'variants', root / 'variants')
            report = root / 'variants' / 'synthetic' / 'report.md'
            report.write_text(report.read_text() + '\nThe stats handler in main.mo stops.\n')
            result = tool('--check', MO, root)
        self.assertEqual(result.returncode, 1)
        for problem in ('SHA256SUMS is stale for report.md', "report.md names 'handler'",
                        "report.md names '.mo'"):
            self.assertIn(problem, result.stdout)

    def test_an_empty_or_foreign_variants_folder_fails(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            (root / 'variants').mkdir()
            self.assertEqual(tool('--check', MO, root).returncode, 1)
            (root / 'variants' / 'stray').mkdir()
            result = tool('--check', MO, root)
        self.assertEqual(result.returncode, 1)
        self.assertIn('no fixture.json, so this is not a variant', result.stdout)

    def test_a_broken_fix_that_passes_is_refused(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            shutil.copytree(TESTDATA / 'variants', root / 'variants')
            variant = root / 'variants' / 'synthetic'
            shutil.copyfile(variant / 'hidden' / 'reference' / 'main.mo',
                            variant / 'hidden' / 'broken' / 'main.mo')
            grade.main(['--freeze', str(variant)])
            result = tool('--check', MO, root)
        self.assertEqual(result.returncode, 1)
        self.assertIn('the broken fix must compile and fail a hidden test: every hidden test '
                      'passed', result.stdout)


def main():
    binaries = sys.argv[1:]
    if not binaries:
        print(__doc__.split('\n\n')[1], file=sys.stderr)
        return 2
    global MO
    ok = True
    for binary in binaries:
        MO = os.path.abspath(binary)
        print(f'phase 4 grader with {binary}', file=sys.stderr)
        suite = unittest.TestSuite()
        for case in (Answers, Blocks, Grading, FixCheck, Check):
            suite.addTests(unittest.defaultTestLoader.loadTestsFromTestCase(case))
        ok = unittest.TextTestRunner(verbosity=1).run(suite).wasSuccessful() and ok
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
