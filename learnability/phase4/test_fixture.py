#!/usr/bin/env python3
"""Tests for learnability/phase4/fixture.py, on the synthetic variant in testdata/, against a
built mo. The live tests start `mo serve`; together they take under a minute.

Run: python3 learnability/phase4/test_fixture.py build/gcc/debug/mo [more mo binaries]
"""

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
import fixture  # noqa: E402

SYNTHETIC = here / 'testdata' / 'variants' / 'synthetic'
MO = None  # set per binary by main


def tool(*args, env=None):
    return subprocess.run([sys.executable, str(here / 'fixture.py'), *map(str, args)],
                          capture_output=True, text=True, check=False, timeout=600,
                          env={**os.environ, **(env or {})})


def variant_with(folder, steps=None, **fields):
    """A copy of the synthetic variant in folder, its fixture.json changed as given."""
    copy = pathlib.Path(folder) / 'variant'
    shutil.copytree(SYNTHETIC, copy)
    data = json.loads((copy / 'fixture.json').read_text())
    if steps is not None:
        data['steps'] = steps
    data.update(fields)
    (copy / 'fixture.json').write_text(json.dumps(data))
    return copy


class Rendering(unittest.TestCase):
    def test_the_committed_prelude_is_what_the_fixture_renders_to(self):
        self.assertEqual(tool('rendered', SYNTHETIC).returncode, 0)
        with tempfile.TemporaryDirectory() as folder:
            copy = variant_with(folder, steps=[{'submit': 'other'}])
            result = tool('rendered', copy)
            self.assertEqual(result.returncode, 1)
            self.assertIn('fixture-steps.mo is not what fixture.json renders to', result.stderr)

    def test_the_prelude_compiles_beside_its_variant(self):
        result = subprocess.run([MO, 'check', 'fixture-steps.mo'], cwd=SYNTHETIC,
                                capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_payload_escapes_survive_rendering(self):
        tricky = 'a "quoted" #{not code} \\ back\nline\ttab\r\x01 é {n}'
        with tempfile.TemporaryDirectory() as folder:
            copy = variant_with(folder, steps=[
                {'submit': tricky},
                {'repeat': 2, 'from': 7, 'steps': [{'submit': 'brief {n}', 'pad': 12}]}])
            self.assertEqual(tool('render', copy).returncode, 0)
            expected = [tricky, 'brief 7'.ljust(12), 'brief 8'.ljust(12)]
            checks = '\n'.join(
                f'  assert page.jobs.get({i}).map(fn(job) job.payload.byte_count end) == '
                f'Some({len(text.encode())})\n'
                f'  assert page.jobs.get({i}).map(fn(job) job.payload.char_count end) == '
                f'Some({len(text)})' for i, text in enumerate(expected))
            (copy / 'escapes.mo').write_text(
                'module Escapes\nuse Main{JobFilter}\nuse FixtureSteps\n\n'
                't0 = Time.parse("2026-01-01T00:00:00Z") or stop "time"\n\n'
                'test "payloads are sent as written"\n'
                '  sim = Simulation.start(Main, t0)\n  FixtureSteps.replay(sim)\n'
                '  assert Simulation.send(sim, Main.list, AllJobs, 0, 10) is Ok(Ok(page))\n'
                f'{checks}\n'
                '  assert page.jobs.first.map(fn(job) job.payload.contains?("\\#{not code}") '
                'end) == Some(true)\nend\n')
            result = subprocess.run([MO, 'test', 'escapes.mo', '--in', 'Escapes'], cwd=copy,
                                    capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_long_calls_and_scoped_leases_render_as_mo_format_and_mo_check_accept(self):
        long_text = 'Brief {n}: ' + 'rebuild the index and post the timings. ' * 3
        with tempfile.TemporaryDirectory() as folder:
            copy = variant_with(folder, steps=[
                {'submit': 'x' * 70, 'pad': 200}, {'submit': 'y' * 120},
                {'repeat': 2, 'steps': [{'submit': long_text, 'pad': 2000}]},
                {'submit': 'z' * 95, 'pad': 2000},
                {'lease': True}, {'repeat': 2, 'steps': [{'heartbeat': 'lease'}]},
                {'repeat': 2, 'steps': [{'lease': True}, {'fail': 'lease'}]}])
            self.assertEqual(tool('render', copy).returncode, 0)
            rendered = (copy / 'fixture-steps.mo').read_text()
            formatted = subprocess.run([MO, 'format', 'fixture-steps.mo'], cwd=copy,
                                       capture_output=True, text=True, timeout=120)
            self.assertEqual(formatted.returncode, 0, formatted.stderr)
            self.assertEqual((copy / 'fixture-steps.mo').read_text(), rendered)
            checked = subprocess.run([MO, 'check', 'fixture-steps.mo'], cwd=copy,
                                     capture_output=True, text=True, timeout=120)
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)

    def test_a_wait_renders_as_the_formatter_writes_it(self):
        self.assertEqual([fixture.duration(s) for s in (1, 20, 60, 120, 1860)],
                         ['1.second', '20.seconds', '1.minute', '2.minutes', '31.minutes'])

    def test_a_malformed_fixture_is_refused_before_any_send(self):
        for steps in ([{'teleport': 1}], [{'lease': True, 'wait': 3}],
                      [{'acknowledge': 'job 3'}], [{'wait': -1}],
                      [{'repeat': 2, 'steps': [{'repeat': 2, 'steps': []}]}],
                      [{'expect': {'call': 'stats', 'error': None}}],
                      [{'expect': {'call': 'acknowledge', 'arguments': {}}}],
                      [{'acknowledge': 'lease'}],
                      [{'repeat': 2, 'steps': [{'lease': True}]}, {'acknowledge': 'lease'}],
                      [{'lease': True},
                       {'repeat': 2, 'steps': [{'acknowledge': 'lease'}, {'lease': True}]}]):
            with tempfile.TemporaryDirectory() as folder:
                copy = variant_with(folder, steps=steps)
                out = pathlib.Path(folder) / 'out'
                result = tool('build', copy, MO, out, 'FIXTOKEN', env={'FIXTOKEN': 'x'})
                self.assertEqual(result.returncode, 2, (steps, result.stderr))
                self.assertTrue(result.stderr.startswith('fixture: '), result.stderr)
                self.assertFalse(out.exists(), steps)


class Live(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.folder.name)
        self.token = secrets.token_hex(16)
        self.env = {'FIXTOKEN': self.token}

    def tearDown(self):
        self.folder.cleanup()

    def build(self, variant, name='out'):
        out = self.root / name
        result = tool('build', variant, MO, out, 'FIXTOKEN', env=self.env)
        return result, out

    def serve(self, variant, data):
        copy = self.root / f'served-{secrets.token_hex(4)}'
        shutil.copytree(data, copy)
        return fixture.Service(MO, variant / 'main.mo', copy, self.token, copy.parent /
                               f'{copy.name}.stderr')

    def test_a_build_fires_its_trigger_and_the_check_passes_on_a_copy(self):
        result, out = self.build(SYNTHETIC)
        self.assertEqual(result.returncode, 0, result.stderr)
        record = json.loads((out / 'build.json').read_text())
        self.assertEqual(len(record['failing_tool_calls']), 1)
        service = self.serve(SYNTHETIC, out / 'data')
        try:
            checked = tool('check', SYNTHETIC, service.url, 'FIXTOKEN', service.stderr_path,
                           env=self.env)
        finally:
            service.stop()
        self.assertEqual(checked.returncode, 0, checked.stderr)
        # The build and one check together call the failing tool twice, no more.
        self.assertEqual(service.stderr_text().count('mo serve: failure: stats'), 1)
        self.assertNotIn(self.token, (out / 'build.json').read_text() + checked.stdout +
                         checked.stderr + result.stdout + result.stderr)

    def test_the_check_refuses_a_folder_whose_trigger_did_not_fire(self):
        quiet = variant_with(self.root, steps=[{'repeat': 4, 'steps': [
            {'submit': 'toy brief {n}'}]}], check=[])
        result, out = self.build(quiet)
        self.assertEqual(result.returncode, 0, result.stderr)
        service = self.serve(SYNTHETIC, out / 'data')
        try:
            checked = tool('check', SYNTHETIC, service.url, 'FIXTOKEN', service.stderr_path,
                           env=self.env)
        finally:
            service.stop()
        self.assertEqual(checked.returncode, 1)
        self.assertIn('job 1 is Ready after 0 attempts, not Dead after 3', checked.stderr)

    def test_a_shared_payload_key_counts_every_job_that_shares(self):
        question = variant_with(self.root, key={'rule': 'shared_payload_jobs'})
        result, out = self.build(question)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads((out / 'key.json').read_text())['answer'], '2')
        again = tool('key', question, MO, out / 'data', 'FIXTOKEN', env=self.env)
        self.assertEqual(again.returncode, 0, again.stderr)
        self.assertEqual(json.loads(again.stdout)['answer'], '2')

    def test_a_killing_call_key_is_the_third_failure_in_the_log(self):
        steps = [{'submit': 'doomed'}, {'submit': 'other'},
                 {'lease': True}, {'fail': 'lease'}, {'lease': True}, {'fail': 'lease'},
                 {'lease': True}, {'wait': 2}, {'fail': 'lease'}]
        question = variant_with(self.root, steps=steps, check=[
            {'job': 1, 'state': 'Dead', 'attempts': 3}],
            key={'rule': 'killing_call', 'job': 1, 'margin': 60}, failing_tool=None)
        before = fixture.rfc3339(int(__import__('time').time() * 1e6))
        result, out = self.build(question)
        self.assertEqual(result.returncode, 0, result.stderr)
        key = json.loads((out / 'key.json').read_text())
        _, records = fixture.log_records(out / 'data')
        fails = [arrival for _, name, arrival, _ in records if name == 'fail']
        self.assertEqual(key['answer'], f'fail {fixture.rfc3339(fails[-1])}')
        self.assertGreaterEqual(key['answer'].split()[1], before)
        self.assertEqual(key['rows_after_snapshot'], len(records))
        tight = variant_with(self.root / 'tight', steps=steps,
                             key={'rule': 'killing_call', 'job': 1, 'margin': 95})
        self.assertIn('leaves fewer than 95 free',
                      tool('key', tight, MO, out / 'data', 'FIXTOKEN', env=self.env).stderr)


def main():
    binaries = sys.argv[1:]
    if not binaries:
        print(__doc__.split('\n\n')[1], file=sys.stderr)
        return 2
    global MO
    ok = True
    for binary in binaries:
        MO = os.path.abspath(binary)
        print(f'phase 4 fixtures with {binary}', file=sys.stderr)
        suite = unittest.TestSuite()
        for case in (Rendering, Live):
            suite.addTests(unittest.defaultTestLoader.loadTestsFromTestCase(case))
        ok = unittest.TextTestRunner(verbosity=1).run(suite).wasSuccessful() and ok
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
