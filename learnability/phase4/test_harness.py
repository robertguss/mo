#!/usr/bin/env python3
"""Tests for learnability/phase4/harness.py: whole sessions of the synthetic variant in testdata/
with the stub client (testdata/stub_client.py) installed as both clients, run as a session
user of their own, mo-session-test, which the tests create when it is absent. They need
passwordless sudo and take a few minutes.

Run: python3 learnability/phase4/test_harness.py build/gcc/debug/mo
"""

import json
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

here = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(here))
import harness  # noqa: E402

SYNTHETIC = here / 'testdata' / 'variants' / 'synthetic'
STUB = here / 'testdata' / 'stub_client.py'
USER = 'mo-session-test'
MO = None  # set by main
SETUP = {}  # what setting up the session user printed, by step
WORK_FILES = ['job-queue.mo', 'main.mo', 'queue-tests.mo']


def harness_command(*args, **kw):
    return subprocess.run([sys.executable, str(here / 'harness.py'), *map(str, args),
                           *LAYOUT], capture_output=True, text=True, timeout=900, **kw)


def write_as_user(path, text):
    """Writes a file in the session user's home as that user."""
    subprocess.run(['sudo', '-n', '-u', USER, 'mkdir', '-p', str(path.parent)], check=True,
                   timeout=60)
    subprocess.run(['sudo', '-n', '-u', USER, 'tee', str(path)], input=text.encode(),
                   stdout=subprocess.DEVNULL, check=True, timeout=60)


def setUpModule():
    global PRIVATE, INSTALL, LAYOUT, ROOT, BUILT
    if subprocess.run(['sudo', '-n', 'true'], capture_output=True).returncode != 0:
        raise RuntimeError('these tests need passwordless sudo')
    PRIVATE = pathlib.Path(tempfile.mkdtemp(prefix='phase4-harness-private-'))
    INSTALL = pathlib.Path(tempfile.mkdtemp(prefix='phase4-harness-install-'))
    os.chmod(INSTALL, 0o755)
    LAYOUT = ['--user', USER, '--install', INSTALL, '--state', PRIVATE / 'state',
              '--private', PRIVATE]
    ROOT, BUILT = PRIVATE / 'root', PRIVATE / 'built'
    shutil.copytree(SYNTHETIC, ROOT / 'variants' / 'synthetic')
    built = harness_command('build', MO, ROOT, BUILT)
    if built.returncode != 0:
        raise RuntimeError(f'build failed: {built.stdout}{built.stderr}')
    unready = ROOT / 'variants' / 'unready'
    shutil.copytree(SYNTHETIC, unready)
    fixture = json.loads((unready / 'fixture.json').read_text())
    fixture['check'] = [{'job': 1, 'state': 'Ready', 'attempts': 0}]
    (unready / 'fixture.json').write_text(json.dumps(fixture))
    shutil.copytree(BUILT / 'synthetic', BUILT / 'unready')

    if subprocess.run(['id', USER], capture_output=True).returncode == 0:
        home = pathlib.Path(harness.Layout(user=USER).home())
        harness.sudo('find', home, '-mindepth', '1', '-delete')
    SETUP['before_login'] = harness_command('setup', MO, STUB, STUB)
    home = harness.Layout(user=USER).home()
    write_as_user(home / '.claude' / '.credentials.json', json.dumps(
        {'claudeAiOauth': {'accessToken': 'sk-stub-access-0123456789abcdef',
                           'refreshToken': 'sk-stub-refresh-0123456789abcdef',
                           'scopes': ['user:inference']}}))
    write_as_user(home / '.codex' / 'auth.json', json.dumps(
        {'OPENAI_API_KEY': None, 'tokens': {'access_token': 'stub-codex-access-0123456789'}}))
    SETUP['after_login'] = harness_command('setup', MO, STUB, STUB)
    if SETUP['after_login'].returncode != 0:
        raise RuntimeError(f'setup failed: {SETUP["after_login"].stderr}')


def tearDownModule():
    if 'LAYOUT' in globals():
        harness_command('reset')
        harness.sudo('rm', '-rf', INSTALL, check=False)
        shutil.rmtree(PRIVATE, ignore_errors=True)


def stub_mode(settings):
    """Tells the stub client what to do; setup made the install folder root's."""
    subprocess.run(['sudo', '-n', 'tee', str(INSTALL / 'stub-mode')],
                   input=json.dumps(settings).encode(), stdout=subprocess.DEVNULL, check=True,
                   timeout=60)


def tokens():
    return (PRIVATE / 'state' / 'tokens').read_text().split()


def session(name, mode='fix', client='claude', variant='synthetic', limit=60, **settings):
    """Runs one stub session into PRIVATE/runs/NAME; returns the finished process and OUT."""
    stub_mode({'mode': mode, **settings})
    out = PRIVATE / 'runs' / name
    result = harness_command('run', ROOT / 'variants' / variant, BUILT / variant, client, out,
                             '--limit', limit)
    return result, out


def manifest(out):
    return json.loads((out / 'manifest.json').read_text())


def verdict(out):
    return json.loads((out / 'grade.json').read_text())


class Setup(unittest.TestCase):
    def test_setup_waits_for_the_logins_then_snapshots_and_keeps_the_credentials(self):
        before = SETUP['before_login']
        self.assertEqual(before.returncode, 1, before.stderr)
        self.assertIn('is not logged in to every client', before.stderr)
        self.assertEqual(SETUP['after_login'].returncode, 0)
        state = PRIVATE / 'state'
        self.assertEqual((state / 'secrets').read_text().split(), [
            'sk-stub-access-0123456789abcdef', 'sk-stub-refresh-0123456789abcdef',
            'stub-codex-access-0123456789'])
        for name in ('secrets', 'tokens', 'home.tar'):
            self.assertEqual(os.stat(state / name).st_mode & 0o777, 0o600, name)
        record = json.loads((state / 'binaries.json').read_text())
        self.assertEqual(record['claude']['version'], 'stub client 1')
        self.assertEqual(record['mo']['sha256'], harness.sha256(MO))
        self.assertEqual(os.stat(INSTALL / 'bin' / 'mo').st_uid, 0)

    def test_setup_refuses_a_home_others_can_search(self):
        with tempfile.TemporaryDirectory() as folder:
            os.chmod(folder, 0o751)
            result = subprocess.run(
                [sys.executable, str(here / 'harness.py'), 'setup', MO, STUB, STUB, '--user',
                 USER, '--install', INSTALL, '--private', folder], capture_output=True,
                text=True, timeout=120)
        self.assertEqual(result.returncode, 2)
        self.assertIn('grants others access', result.stderr)

    def test_a_folder_is_reachable_by_its_others_or_its_group_bits(self):
        with tempfile.TemporaryDirectory() as folder:
            inner = pathlib.Path(folder) / 'a' / 'b'
            inner.mkdir(parents=True)
            os.chmod(folder, 0o755)
            os.chmod(inner.parent, 0o755)
            self.assertTrue(harness.reachable_by(inner, USER))
            os.chmod(inner.parent, 0o750)
            self.assertFalse(harness.reachable_by(inner, USER))
            harness.sudo('chgrp', USER, inner.parent)
            self.assertTrue(harness.reachable_by(inner, USER))
            harness.sudo('chmod', '0700', inner.parent)
            self.assertFalse(harness.reachable_by(inner, USER))
            harness.sudo('chgrp', str(os.getgid()), inner.parent)

    def test_the_order_is_reproducible_and_balanced(self):
        names = [f'v{i}' for i in range(4)]
        first = harness.order('20261107', names)
        self.assertEqual(first, harness.order('20261107', names))
        self.assertNotEqual(first, harness.order('20261108', names))
        self.assertEqual(len(first), 16)
        for name in names:
            for client in harness.CLIENTS:
                self.assertEqual(first.count((name, client)), 2)

    def test_each_client_is_given_its_model_and_effort_on_the_command_line(self):
        claude = harness.client_command('claude', 'claude', 'PROMPT', 'http://x/mcp', 'c.json')
        self.assertEqual(claude[claude.index('--model') + 1], 'claude-opus-5-5')
        codex = harness.client_command('codex', 'codex', 'PROMPT', 'http://x/mcp', 'c.json')
        self.assertEqual(codex[codex.index('-m') + 1], 'gpt-6.1-sol')
        self.assertIn('model_reasoning_effort="xhigh"', codex)
        self.assertEqual(codex[-1], 'PROMPT')

    def test_setup_for_another_mo_keeps_the_earlier_record(self):
        state = PRIVATE / 'state'
        earlier = json.loads((state / 'binaries.json').read_text())
        try:
            other = harness_command('setup', STUB, STUB, STUB)
            self.assertEqual(other.returncode, 0, other.stderr)
            record = json.loads((state / 'binaries.json').read_text())
            self.assertEqual(record['mo']['sha256'], harness.sha256(STUB))
            self.assertIn({k: earlier[k] for k in ('mo', 'claude', 'codex')}, record['previous'])
        finally:
            again = harness_command('setup', MO, STUB, STUB)
        self.assertEqual(again.returncode, 0, again.stderr)
        record = json.loads((state / 'binaries.json').read_text())
        self.assertEqual(record['mo']['sha256'], harness.sha256(MO))
        self.assertEqual([p['mo']['sha256'] for p in record['previous']], [harness.sha256(STUB)])


class Sessions(unittest.TestCase):
    def assert_no_token(self, out, *texts):
        token = tokens()[-1]
        pieces = {token[i:i + 16] for i in range(len(token) - 15)}
        files = [p for p in out.rglob('*') if p.is_file()]
        for text in [p.read_bytes().decode(errors='replace') for p in files] + list(texts):
            self.assertFalse(any(piece in text for piece in pieces))

    def assert_session_user_idle(self):
        self.assertEqual(harness.processes(USER), [])

    def test_a_stub_session_is_recorded_matched_and_graded(self):
        for client in harness.CLIENTS:
            before = len(tokens())
            result, out = session(f'fix-{client}', client=client)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(len(tokens()), before + 1)
            seen = manifest(out)
            self.assertEqual(
                {k: seen[k] for k in ('killed_at_limit', 'client_exit_status',
                                      'service_exit_status', 'service_stopped_by_itself',
                                      'first_assistant_event',
                                      'check_copy_removed_before_launch', 'failed')},
                {'killed_at_limit': False, 'client_exit_status': 0, 'service_exit_status': 0,
                 'service_stopped_by_itself': False, 'first_assistant_event': True,
                 'check_copy_removed_before_launch': True, 'failed': []})
            for name in ('stream.jsonl', 'proxy.jsonl', 'service.log', 'prompt.txt',
                         'task-queue/main.mo', 'start-data/service.sqlite'):
                self.assertEqual(seen['artifacts'][name], harness.sha256(out / name), name)
            self.assertEqual(sorted(os.listdir(out / 'task-queue')), WORK_FILES)
            self.assertEqual((out / 'start-data' / 'service.sqlite').read_bytes(),
                             (BUILT / 'synthetic' / 'data' / 'service.sqlite').read_bytes())
            graded = verdict(out)
            self.assertEqual((graded['success'], graded['grader']['reasons']), (True, []))
            self.assertEqual((graded['records']['match'], graded['records']['pairs']), (True, 2))
            self.assertEqual(graded['coverage'], {'operator_tools': [], 'commands': []})
            self.assertEqual({k: graded['fix_check'][k] for k in
                              ('original', 'verdict', 'violation', 'graded_correct')},
                             {'original': 'reproduced', 'verdict': 'verified', 'violation': False,
                              'graded_correct': True})
            self.assertEqual(seen['command'][0], str(INSTALL / 'bin' / client))
            self.assertIn('PROMPT', seen['command'])
            prompt = (out / 'prompt.txt').read_text()
            self.assertIn((SYNTHETIC / 'report.md').read_text().strip(), prompt)
            self.assertNotIn('synthetic', prompt)
            self.assert_no_token(out, result.stdout, result.stderr)
            self.assertEqual(sorted(os.listdir(PRIVATE / 'state')),
                             ['binaries.json', 'home.tar', 'leftover-roots.json', 'secrets',
                              'tokens'])
            self.assert_session_user_idle()

    def test_the_limit_ends_the_whole_tree_and_the_half_edit_is_graded(self):
        result, out = session('sleep', mode='sleep', limit=20)
        self.assertEqual(result.returncode, 1, result.stderr)
        seen = manifest(out)
        self.assertTrue(seen['killed_at_limit'])
        self.assertGreaterEqual(seen['seconds'], 20)
        self.assertLess(seen['seconds'], 20 + harness.GRACE + 15)
        self.assertTrue((out / 'task-queue' / 'main.mo').read_text().endswith(
            '## half-edited\nfn half('))
        self.assertFalse(verdict(out)['grader']['success'])
        self.assert_session_user_idle()

    def test_a_service_the_session_stopped_stays_stopped(self):
        result, out = session('trip', mode='trip')
        self.assertEqual(result.returncode, 1, result.stderr)
        seen = manifest(out)
        self.assertEqual((seen['service_exit_status'], seen['service_stopped_by_itself']),
                         (69, True))
        self.assertIn('mo serve: failure: stats', (out / 'service.log').read_text())
        self.assertTrue(verdict(out)['records']['match'], verdict(out)['records'])
        statuses = [e['status'] for e in harness.proxy.entries(out / 'proxy.jsonl')
                    if e['direction'] == 'response']
        self.assertEqual(statuses[-1], 502)

    def test_a_command_addressing_another_host_fails_the_session(self):
        result, out = session('foreign', mode='foreign')
        self.assertEqual(result.returncode, 1, result.stderr)
        failed = ['addressed example.com: curl -s https://example.com/install.sh']
        self.assertEqual(manifest(out)['failed'], failed)
        self.assertEqual(verdict(out)['failed'], failed)

    def test_a_failed_fixture_check_discards_the_session_before_any_client(self):
        before = len(tokens())
        result, out = session('unready', variant='unready')
        self.assertEqual(result.returncode, 3, result.stderr)
        self.assertEqual(os.listdir(out), ['discarded.json'])
        self.assertIn('job 1 is Dead after 3 attempts, not Ready after 0',
                      json.loads((out / 'discarded.json').read_text())['check_failed'])
        self.assertEqual(len(tokens()), before + 1)
        self.assert_session_user_idle()

    def test_a_session_finds_nothing_an_earlier_one_left_and_none_of_the_private_files(self):
        result, _ = session('litter', mode='litter')
        self.assertEqual(result.returncode, 1, result.stderr)
        repository_file = here / 'harness.py'
        private = [PRIVATE / 'state' / 'tokens', PRIVATE / 'state', ROOT, ROOT / 'variants' /
                   'unready' / 'hidden' / 'hidden-tests.mo', BUILT / 'synthetic' / 'key.json',
                   BUILT, PRIVATE / 'runs' / 'litter' / 'stream.jsonl']
        layout = harness.Layout(user=USER, install=INSTALL)
        result, out = session('look', mode='look', start_data=str(layout.start_data),
                              paths=[str(p) for p in private + [repository_file]])
        message = harness.grade.last_message(out / 'stream.jsonl')
        self.assertTrue(message.startswith('{'), (out / 'client-stderr.txt').read_text())
        found = json.loads(message)
        self.assertEqual(found['readable'], {
            **{str(p): False for p in private},
            str(repository_file): harness.reachable_by(repository_file, USER)})
        self.assertEqual((found['home_litter'], found['tmp_litter'], found['lock_litter'],
                          found['sleepers']), (False, False, False, 0))
        self.assertEqual(found['work'], WORK_FILES)
        self.assertEqual((found['wrote_start_data'], found['start_data']),
                         (False, ['service.sqlite']))
        self.assert_session_user_idle()

    def test_a_harness_killed_mid_session_has_already_kept_the_token(self):
        before = len(tokens())
        stub_mode({'mode': 'sleep'})
        out = PRIVATE / 'runs' / 'killed'
        process = subprocess.Popen(
            [sys.executable, str(here / 'harness.py'), 'run', ROOT / 'variants' / 'synthetic',
             BUILT / 'synthetic', 'claude', out, '--limit', '600', *LAYOUT],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        self.wait_for_client(out)
        os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=60)
        self.assertEqual(len(tokens()), before + 1)
        self.assertFalse((out / 'manifest.json').exists())
        reset = harness_command('reset')
        self.assertEqual(reset.returncode, 0, reset.stderr)
        self.assert_session_user_idle()

    def test_a_service_that_cannot_start_discards_the_session(self):
        broken = ROOT / 'variants' / 'unservable'
        shutil.copytree(SYNTHETIC, broken)
        (broken / 'main.mo').write_text('module Main\nnot a program\n')
        shutil.copytree(BUILT / 'synthetic', BUILT / 'unservable')
        try:
            result, out = session('unservable', variant='unservable')
        finally:
            shutil.rmtree(broken)
            shutil.rmtree(BUILT / 'unservable')
        self.assertEqual(result.returncode, 3, result.stderr)
        self.assertIn('mo serve exited', json.loads(
            (out / 'discarded.json').read_text())['check_failed'])
        self.assert_session_user_idle()

    def test_a_session_can_fetch_read_history_and_check_a_fix_against_the_deployed_source(self):
        # mo reads its library from the tree it was built in, so replay as the session user
        # needs an mo built where that user can read, as the evaluation's candidate is.
        tree = pathlib.Path(tempfile.mkdtemp(prefix='phase4-harness-tree-'))
        try:
            os.chmod(tree, 0o755)
            listed = subprocess.run(['git', '-C', str(here.parents[1]), 'ls-files', '-z'],
                                    capture_output=True, check=True).stdout
            for name in listed.decode().split('\0')[:-1]:
                source = here.parents[1] / name
                if source.is_file():
                    (tree / name).parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(source, tree / name)
            subprocess.run(['make', '-s', '-C', str(tree), f'-j{os.cpu_count()}', 'CC=gcc',
                            'build/gcc/debug/mo'], check=True, capture_output=True, timeout=1800)
            candidate = tree / 'build' / 'gcc' / 'debug' / 'mo'
            self.assertEqual(harness_command('setup', candidate, STUB, STUB).returncode, 0)
            root, built = PRIVATE / 'root-candidate', PRIVATE / 'built-candidate'
            shutil.copytree(SYNTHETIC, root / 'variants' / 'synthetic')
            made = harness_command('build', candidate, root, built)
            self.assertEqual(made.returncode, 0, made.stdout + made.stderr)
            self.instruments(built)
        finally:
            self.assertEqual(harness_command('setup', MO, STUB, STUB).returncode, 0)
            shutil.rmtree(tree, ignore_errors=True)

    def instruments(self, built):
        for client in harness.CLIENTS:
            stub_mode({'mode': 'instruments', 'data': str(INSTALL / 'session' / 'start-data'),
                       'deployed': str(INSTALL / 'session' / 'deployed')})
            out = PRIVATE / 'runs' / f'instruments-{client}'
            result = harness_command('run', ROOT / 'variants' / 'synthetic', built / 'synthetic',
                                     client, out, '--limit', 120)
            self.assertEqual(result.returncode, 0, result.stderr)
            graded = verdict(out)
            self.assertTrue(graded['success'], graded)
            self.assertEqual((graded['records']['pairs'], graded['records']['log_only']),
                             (1, []))
            self.assertGreater(graded['records']['shell'], 0)
            self.assertEqual(graded['coverage'], {
                'operator_tools': ['mo-overview'],
                'commands': ['fetch', 'history', 'replay', 'replay --fix']})
            reported = json.loads(harness.grade.last_message(out / 'stream.jsonl'))
            self.assertEqual(reported, {'fetch': 0, 'history': 0, 'replay': 0,
                                        'verdicts': ['verified'], 'wrote_deployed': False})
            self.assertFalse((INSTALL / 'session' / 'deployed').exists())
            self.assert_session_user_idle()

    def test_a_round_built_by_another_mo_is_refused(self):
        other = PRIVATE / 'built-elsewhere'
        shutil.copytree(BUILT / 'synthetic', other / 'synthetic')
        (other / 'round.json').write_text(json.dumps({'mo_sha256': '0' * 64}))
        result = harness_command('run', ROOT / 'variants' / 'synthetic', other / 'synthetic',
                                 'claude', PRIVATE / 'runs' / 'elsewhere')
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn('was built by another mo', result.stderr)
        self.assertFalse((PRIVATE / 'runs' / 'elsewhere').exists())

    def test_a_tool_call_the_proxy_never_saw_fails_the_session(self):
        result, out = session('unrecorded', mode='unrecorded')
        self.assertEqual(result.returncode, 1, result.stderr)
        graded = verdict(out)
        self.assertEqual((graded['success'], graded['grader']['success']), (False, True))
        self.assertFalse(graded['records']['match'])
        self.assertEqual(graded['records']['stream_only'],
                         [{'stream_id': 'toolu_lost', 'tool': 'stats'}])

    def test_a_refreshed_login_is_kept_for_the_scanner_and_the_next_session(self):
        secret = 'sk-stub-refreshed-' + os.urandom(12).hex()
        result, _ = session('refresh', mode='refresh', secret=secret)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(secret, (PRIVATE / 'state' / 'secrets').read_text().split())
        home = harness.Layout(user=USER).home()
        login = harness.sudo('cat', home / '.claude' / '.credentials.json').stdout.decode()
        self.assertIn(secret, login)

    def test_a_harness_stopped_by_sigterm_ends_the_session(self):
        stub_mode({'mode': 'sleep'})
        out = PRIVATE / 'runs' / 'terminated'
        process = subprocess.Popen(
            [sys.executable, str(here / 'harness.py'), 'run', ROOT / 'variants' / 'synthetic',
             BUILT / 'synthetic', 'claude', out, '--limit', '600', *LAYOUT],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self.wait_for_client(out)
        process.send_signal(signal.SIGTERM)
        _, stderr = process.communicate(timeout=120)
        self.assertNotEqual(process.returncode, 0)
        self.assertIn(b'stopped by signal', stderr)
        self.assert_session_user_idle()

    def wait_for_client(self, out):
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            stream = out / 'stream.jsonl'
            if stream.exists() and '"assistant"' in stream.read_text():
                return
            time.sleep(0.5)
        self.fail('the stub client never started')

    def test_a_round_reruns_a_slot_whose_client_never_answered(self):
        order = PRIVATE / 'silent-order.tsv'
        order.write_text('1\tsynthetic\tclaude\n')
        out = PRIVATE / 'silent-round'
        stub_mode({'mode': 'silent'})
        first = harness_command('round', order, ROOT, BUILT, out)
        self.assertEqual(first.returncode, 4, first.stderr)
        self.assertEqual(sorted(os.listdir(out)), ['unstarted'])
        stub_mode({'mode': 'fix'})
        second = harness_command('round', order, ROOT, BUILT, out)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertTrue(verdict(out / '01-synthetic-claude')['success'])

    def test_a_round_keeps_a_discarded_slot_for_the_repaired_fixture(self):
        order = PRIVATE / 'order.tsv'
        order.write_text('# seed test\n1\tunready\tclaude\n2\tsynthetic\tclaude\n')
        out = PRIVATE / 'round'
        stub_mode({'mode': 'fix'})
        first = harness_command('round', order, ROOT, BUILT, out)
        self.assertEqual(first.returncode, 3, first.stderr)
        self.assertEqual(sorted(os.listdir(out)), ['01-unready-claude'])
        unready = ROOT / 'variants' / 'unready'
        shutil.copyfile(SYNTHETIC / 'fixture.json', unready / 'fixture.json')
        try:
            second = harness_command('round', order, ROOT, BUILT, out)
        finally:
            fixture = json.loads((unready / 'fixture.json').read_text())
            fixture['check'] = [{'job': 1, 'state': 'Ready', 'attempts': 0}]
            (unready / 'fixture.json').write_text(json.dumps(fixture))
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(sorted(os.listdir(out)),
                         ['01-unready-claude', '02-synthetic-claude', 'discarded'])
        self.assertEqual(len(os.listdir(out / 'discarded')), 1)
        for slot in ('01-unready-claude', '02-synthetic-claude'):
            self.assertTrue(verdict(out / slot)['success'], slot)
        before = len(tokens())
        third = harness_command('round', order, ROOT, BUILT, out)
        self.assertEqual((third.returncode, len(tokens())), (0, before))


def main():
    if len(sys.argv) != 2:
        print(__doc__.split('\n\n')[1], file=sys.stderr)
        return 2
    global MO
    MO = os.path.abspath(sys.argv[1])
    print(f'phase 4 session harness with {sys.argv[1]}', file=sys.stderr)
    suite = unittest.TestSuite()
    for case in (Setup, Sessions):
        suite.addTests(unittest.defaultTestLoader.loadTestsFromTestCase(case))
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1


if __name__ == '__main__':
    sys.exit(main())
