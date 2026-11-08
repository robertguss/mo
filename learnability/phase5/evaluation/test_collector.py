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
    def test_development_and_timeout(self):
        p4 = harness.p4
        p4.sudo('true')
        p4.sudo('iptables', '-L', 'OUTPUT', '-n')
        private = Path(tempfile.mkdtemp(prefix='phase5-test-private-'))
        install = Path(tempfile.mkdtemp(prefix='phase5-test-install-'))
        install.chmod(0o755)
        l = p4.Layout(user='mo-phase5-test', install=install, private=private, state=private/'state')
        mo = Path(os.environ['MO_EVALUATION_BINARY']).resolve()
        stub = harness.HERE / 'stub_client.py'
        try:
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                p4.setup(l, mo, stub, stub)
                for name, value in {'.claude/.credentials.json': {'claudeAiOauth': {'accessToken': 'stub-only-claude-0123456789'}}, '.codex/auth.json': {'tokens': {'access_token': 'stub-only-codex-0123456789'}}}.items():
                    dest = l.home()/name
                    p4.sudo('install', '-d', '-o', l.user, '-g', l.user, dest.parent)
                    import subprocess
                    subprocess.run(['sudo', '-n', '-u', l.user, 'tee', str(dest)], input=json.dumps(value).encode(), stdout=subprocess.DEVNULL, check=True)
                    p4.sudo('chown', '-R', l.user+':'+l.user, dest.parent)
                self.assertEqual(p4.setup(l, mo, stub, stub), 0)
            fixture = harness.grader.fixture
            fixture.build(mo, private/'fixture', 'fixture-service-only', 'fixture-upgrade-only')
            reference = fixture.sources(private/'reference')
            (reference/'upgrade-old.mo').write_text((private/'fixture/old/job-queue.mo').read_text().replace('module JobQueue', 'module UpgradeOld').replace('expose Queue,', 'expose Queue(..),'))
            p4.sudo('cp', '-r', reference, install/'reference')
            p4.sudo('chmod', '-R', 'a+rX', install/'reference')
            prompt = private/'prompt'
            prompt.write_text('Upgrade the queue at SERVICE_URL.')
            baseline = p4.sudo('iptables', '-S', 'OUTPUT').stdout
            for case in ('success', 'timeout', 'cleanup-failure'):
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
                        settings = {'hidden': str(private), 'port': int(args[args.index('--dport')+1]), 'sleep': sleeping}
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
                with patch.object(p4, 'sudo', sudo), patch.object(p4, 'reset', reset):
                    status = harness.collect(l, path, out, 1, development=True, limit=3 if sleeping else 60)
                meta = json.loads((out/'manifest.json').read_text())
                with self.subTest(case=case):
                    self.assertEqual(status, int(sleeping or cleanup_failure), json.dumps(meta) + (out/'client-stderr.txt').read_text() + (out/'source-tests.json').read_text())
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
                        self.assertTrue((captured/'partial.txt').exists())
                    else:
                        self.assertTrue(meta['records']['match'])
                        self.assertTrue(meta['grade']['correct'])
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
            shutil.rmtree(private)


if __name__ == '__main__':
    unittest.main()
