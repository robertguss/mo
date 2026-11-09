"""Real OS collector exercise, fake login fixtures only. Requires sudo/iptables."""
import contextlib
import io
import json
import os
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch
import harness


class Collector(unittest.TestCase):
    def test_missing_report_continues_state_grading(self):
        from types import SimpleNamespace
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            layout = SimpleNamespace(work=Path('work'), bin=Path('/unused'))
            meta = {'client': 'codex', 'development': True, 'failures': []}
            with patch.object(harness.grader, 'source_precheck', return_value=SimpleNamespace(returncode=0, stdout='')),                 patch.object(harness.grader, 'attestation', return_value=None),                 patch.object(harness.grader, 'accepted_report', return_value=None),                 patch.object(harness, 'final_message', return_value='No upgrade attempted'),                 patch.object(harness.grader, 'stopped_grade', return_value={'reasons': [], 'dimensions': {}}) as stopped:
                harness.grade_collected(layout, out, {}, {}, 'token', 'upgrade', meta)
            stopped.assert_called_once()
            self.assertIn('no accepted upgrade with a valid migration selector', meta['failures'])
            self.assertIsNone(meta['identity']['candidate_source'])

    def test_missing_binary_does_not_allocate(self):
        with patch.dict(os.environ, {}, clear=True), patch.object(tempfile, 'mkdtemp') as allocate:
            with self.assertRaises(KeyError):
                self.test_development_and_timeout()
            allocate.assert_not_called()

    def test_development_and_timeout(self, cases=('success', 'curl', 'urllib', 'timeout', 'cleanup-failure')):
        mo = Path(os.environ['MO_EVALUATION_BINARY']).resolve()
        p4 = harness.p4
        p4.sudo('true')
        p4.sudo('iptables', '-L', 'OUTPUT', '-n')
        private = Path(tempfile.mkdtemp(prefix='phase5-test-private-', dir=Path.home()))
        install = Path(tempfile.mkdtemp(prefix='phase5-test-install-', dir=Path.home()))
        install.chmod(0o755)
        l = p4.Layout(user='mo-phase5-test', install=install, private=private, state=private/'state')
        stub = harness.HERE / 'stub_client.py'
        home_mode = Path.home().stat().st_mode & 0o777
        Path.home().chmod(home_mode | 0o001)
        try:
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                harness.setup(l, mo, stub, stub)
                for name, value in {'.claude/.credentials.json': {'claudeAiOauth': {'accessToken': 'stub-only-claude-0123456789'}}, '.codex/auth.json': {'tokens': {'access_token': 'stub-only-codex-0123456789'}}}.items():
                    dest = l.home()/name
                    p4.sudo('install', '-d', '-o', l.user, '-g', l.user, dest.parent)
                    import subprocess
                    subprocess.run(['sudo', '-n', '-u', l.user, 'tee', str(dest)], input=json.dumps(value).encode(), stdout=subprocess.DEVNULL, check=True)
                    p4.sudo('chown', '-R', l.user+':'+l.user, dest.parent)
                self.assertEqual(harness.setup(l, mo, stub, stub), 0)
            self.assertEqual((l.bin / 'mo').stat().st_mode & 0o777, 0o555)
            fixture = harness.grader.fixture
            fixture.build(mo, private/'fixture', 'fixture-service-only', 'fixture-upgrade-only')
            reference = fixture.sources(private/'reference')
            (reference/'upgrade-old.mo').write_text((private/'fixture/old/job-queue.mo').read_text().replace('module JobQueue', 'module UpgradeOld').replace('expose Queue,', 'expose Queue(..),'))
            p4.sudo('cp', '-r', reference, install/'reference')
            p4.sudo('chmod', '-R', 'a+rX', install/'reference')
            prompt = private/'prompt'
            prompt.write_text('Upgrade the queue at SERVICE_URL.')
            baseline = p4.sudo('iptables', '-S', 'OUTPUT').stdout
            for case in cases:
                sleeping = case == 'timeout'
                cleanup_failure = case == 'cleanup-failure'
                spec = {k: str(v) for k, v in dict(source=private/'fixture/old', context=private/'fixture/old', data=private/'fixture/data', expected=private/'fixture/expected.json', prompt=prompt, experiment=private/('experiment-'+case)).items()}
                path = private/'spec.json'
                path.write_text(json.dumps(spec))
                out = private/('run-'+case)
                original = p4.sudo
                def sudo(*args, **kwargs):
                    result = original(*args, **kwargs)
                    if args[:2] == ('iptables', '-I'):
                        settings = {'hidden': str(private), 'port': int(args[args.index('--dport')+1]), 'sleep': sleeping, 'noop': case == 'noop', 'compile-failure': case == 'compile-failure',
                                    'query': case if case in ('curl', 'urllib') else 'native'}
                        import subprocess
                        subprocess.run(['sudo', '-n', 'tee', str(install/'stub-settings.json')], input=json.dumps(settings).encode(), stdout=subprocess.DEVNULL, check=True)
                    return result
                original_reset = p4.reset
                resets = 0
                def reset(installed):
                    nonlocal resets
                    original_reset(installed)
                    resets += 1
                    if cleanup_failure and resets == 2:
                        raise RuntimeError('injected reset reporting failure')
                def fail_bundle(path, *args, **kwargs):
                    Path(path).mkdir(mode=0o700)
                    raise ValueError('injected full-bundle construction failure')
                retention_fault = (patch.object(harness.replay_bundle, 'Bundle', side_effect=fail_bundle)
                                   if case == 'retention-failure' else contextlib.nullcontext())
                with patch.object(p4, 'sudo', sudo), patch.object(p4, 'reset', reset), retention_fault:
                    status = harness.collect(l, path, out, 1, development=True, limit=3 if sleeping else 60)
                meta = json.loads((out/'manifest.json').read_text())
                with self.subTest(case=case):
                    self.assertEqual(status, 1, json.dumps(meta) + (out/'client-stderr.txt').read_text() + (out/'source-tests.json').read_text())
                    self.assertEqual(meta['evidence_class'], 'DEVELOPMENT')
                    self.assertTrue(meta['development'])
                    self.assertEqual(meta['timeout'], sleeping)
                    if cleanup_failure:
                        self.assertFalse(meta['correct'])
                        self.assertTrue(any('cleanup reset' in f for f in meta['failures']))
                    self.assertEqual(p4.processes(l.user), [])
                    self.assertEqual(p4.sudo('iptables', '-S', 'OUTPUT').stdout, baseline)
                    captured = out/l.work.name
                    self.assertTrue(all(json.loads((captured/'isolation.json').read_text()).values()))
                    if sleeping:
                        self.assertFalse(meta['capture']['capture_finished'])
                        self.assertFalse(meta['records']['match'])
                        self.assertTrue((captured/'partial.txt').exists())
                    elif case == 'retention-failure':
                        self.assertTrue(any('record reconciliation' in f for f in meta['failures']))
                        self.assertTrue((out/'replay-partial-private/manifest.json').is_file())
                        self.assertEqual(meta['replay_path'], str(out/'replay-partial-private'))
                        self.assertFalse(meta['dimensions']['traffic_completeness'])
                        self.assertFalse(meta['correct'])
                    elif case == 'noop':
                        self.assertFalse(meta['correct'])
                        self.assertFalse(meta['dimensions']['activation_identity'])
                        self.assertEqual(meta['records']['verified_push_requests'], 0)
                        self.assertNotIn('stub-push', (out/'stream.jsonl').read_text())
                    elif case == 'compile-failure':
                        self.assertTrue(meta['records']['match'])
                        self.assertEqual(meta['records']['verified_push_requests'], 6)
                        self.assertFalse(meta['correct'])
                        self.assertFalse(meta['grade']['correct'])
                        for dimension in ('task_state', 'command_origin', 'traffic_completeness', 'final_answer'):
                            self.assertTrue(meta['dimensions'][dimension], dimension)
                        for dimension in ('activation_identity', 'source_tests', 'model_provenance'):
                            self.assertFalse(meta['dimensions'][dimension], dimension)
                        self.assertIn('@@@ invalid development syntax', (captured/'job-queue.mo').read_text())
                    else:
                        self.assertTrue(meta['records']['match'], meta)
                        self.assertEqual(meta['records']['verified_push_requests'], 6)
                        self.assertTrue(meta['grade']['correct'], meta)
                        self.assertFalse(meta['correct'])
                        for dimension in ('task_state', 'activation_identity', 'source_tests', 'command_origin', 'traffic_completeness', 'final_answer'):
                            self.assertTrue(meta['dimensions'][dimension], meta)
                        for dimension in ('model_provenance', 'publication_integrity'):
                            self.assertFalse(meta['dimensions'][dimension], meta)
                        self.assertNotIn('no successful recorded command-origin push', meta['failures'])
                        self.assertNotIn('post-activation revision observation not proven', meta['failures'])
                        self.assertNotIn('activation identity mismatch', meta['failures'])
                        self.assertEqual(meta['grade']['evidence_class'], 'DEVELOPMENT')
                        self.assertEqual((captured/'main.mo').read_bytes(), (reference/'main.mo').read_bytes())
                        self.assertTrue((out/'source-tests.json').exists())
                        stream = (out/'stream.jsonl').read_text()
                        for token in l.tokens.read_text().split():
                            self.assertNotIn(token, stream)
                        self.assertIn('--token-env QUEUE_TOKEN', stream)
                        self.assertIn('--upgrade-token-env QUEUE_UPGRADE', stream)

        finally:
            if l.snapshot.exists():
                p4.reset(l)
            p4.sudo('rm', '-rf', install)
            # Retain protected evidence, including failed controls, for review.
            (private / 'retained-control.txt').write_text('Development control; never publish raw traces.\n')
            Path.home().chmod(home_mode)


if __name__ == '__main__':
    unittest.main()
