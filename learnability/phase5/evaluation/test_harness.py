import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('runner', Path(__file__).with_name('harness.py'))
h = importlib.util.module_from_spec(spec)
spec.loader.exec_module(h)

class RunnerTests(unittest.TestCase):
    def test_setup_makes_capture_binary_read_only(self):
        from types import SimpleNamespace
        installed = SimpleNamespace(bin=Path('/protected/bin'), state=Path('/nonexistent-test-state'), work=Path('/nonexistent-test-work'))
        for status in (0, 1):
            with patch.object(h.p4, 'setup', return_value=status) as setup, patch.object(h.p4, 'sudo') as sudo:
                self.assertEqual(h.setup(installed, 'mo', 'claude', 'codex'), status)
                setup.assert_called_once_with(installed, 'mo', 'claude', 'codex')
                sudo.assert_called_once_with('chmod', '0555', installed.bin / 'mo')

    def test_native_occurrences_are_consumed_once(self):
        with tempfile.TemporaryDirectory() as tmp:
            stream, log = Path(tmp) / 'stream', Path(tmp) / 'log'
            rows = []
            for n in range(2):
                rows.extend([dict(seq=2*n+1, direction='request', rpc='tools/call', id=1,
                                  tool='revision', body={'params': {'arguments': {}}}),
                             dict(seq=2*n+2, direction='response', request=2*n+1)])
            log.write_text(''.join(json.dumps(r) + '\n' for r in rows))
            for client in ('codex', 'claude'):
                events = []
                for n in range(2):
                    if client == 'codex':
                        item = dict(id=str(n), type='mcp_tool_call', server='task-queue',
                                    tool='revision', arguments={})
                        events += [dict(type='item.started', item=item),
                                   dict(type='item.completed', item=item)]
                    else:
                        events += [dict(type='assistant', message={'content': [dict(
                            type='tool_use', id=str(n), name='mcp__task-queue__revision', input={})]}),
                            dict(type='user', message={'content': [dict(
                                type='tool_result', tool_use_id=str(n), content='1')]})]
                stream.write_text(''.join(json.dumps(e) + '\n' for e in events))
                matched = h.proxy.match(client, stream, log)
                self.assertTrue(matched['match'], matched)
                self.assertEqual(matched['pairs'], 2)
                self.assertEqual(matched['command_origin'], [])
                log.write_text(''.join(json.dumps(r) + '\n' for r in rows + [
                    dict(rows[0], seq=5), dict(rows[1], seq=6, request=5)]))
                self.assertFalse(h.proxy.match(client, stream, log)['match'])
                log.write_text(''.join(json.dumps(r) + '\n' for r in rows))

    def test_local_imports_are_isolated(self):
        self.assertEqual(Path(h.grader.fixture.__file__).parent, h.HERE)
        self.assertEqual(Path(h.p4.fixture.__file__).parent.name, 'phase4')
        self.assertIs(h.grader.fixture.proxy, h.proxy)

    def test_launch_carries_both_credentials_and_stops_at_the_limit(self):
        from types import SimpleNamespace
        launched = []
        class Child:
            waits = 0
            def __init__(self, argv, **kwargs):
                launched.append((argv, kwargs['env']))
            def wait(self, timeout):
                Child.waits += 1
                if Child.waits == 1:
                    raise h.subprocess.TimeoutExpired('client', timeout)
                return -15
        with tempfile.TemporaryDirectory() as tmp, patch.object(h.subprocess, 'Popen', Child), \
                patch.object(h.p4, 'signal_all') as signal_all:
            env = {'QUEUE_TOKEN': 't', 'QUEUE_UPGRADE': 'u'}
            self.assertEqual(h.launch(SimpleNamespace(user='session'), ['client', 'arg'], env, Path(tmp), 7), (-15, True))
            self.assertTrue((Path(tmp) / 'stream.jsonl').exists())
            self.assertTrue((Path(tmp) / 'client-stderr.raw').exists())
        argv, given = launched[0]
        self.assertEqual(argv, ['sudo', '-n', '-u', 'session', '--preserve-env=QUEUE_TOKEN,QUEUE_UPGRADE',
                                '--', 'client', 'arg'])
        self.assertIs(given, env)
        signal_all.assert_called_once_with('session', 'TERM')

    def test_recorded_evidence_is_never_graded(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            push = "/bin/bash -lc 'cd /work && mo push main.mo --url URL'"
            for command, shown in ((push, True), ('mo check main.mo', False)):
                (out / 'stream.jsonl').write_text(json.dumps({'type': 'item.completed', 'item': {
                    'type': 'command_execution', 'command': command}}) + '\n')
                (out / 'client-stderr.raw').write_text('')
                matched = dict(log_only=[{'tool': 'mo-upgrade-open'}], upload_routes=dict(native=0, shell=1))
                record = h.recorded('codex', out, matched)
                self.assertIs(record['transcript_mo_push'], shown)
                self.assertEqual(record['reported_models'], [])
                self.assertEqual(record['requested_model'], 'gpt-6.1-sol')
                self.assertEqual(record['unmatched_proxy_calls'], 1)
                self.assertEqual(record['upload_routes'], dict(native=0, shell=1))

    def test_reservation_order_and_no_reruns(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            experiment = root / 'experiment'
            out = root / 'out'
            out.mkdir()
            with self.assertRaises(ValueError):
                h.reserve(experiment, 2, out)
            h.reserve(experiment, 1, out)
            with self.assertRaises(FileExistsError):
                h.reserve(experiment, 1, out)
            with self.assertRaises(ValueError):
                h.reserve(experiment, 2, out)
            (out / 'manifest.json').write_text('{}')
            h.reserve(experiment, 2, root / 'second')

    def test_layout_is_distinct(self):
        l = h.layout()
        self.assertEqual(l.user, 'mo-phase5-session')
        self.assertNotIn('phase4', str(l.state))
        self.assertNotEqual(l.install, h.p4.Layout().install)

    def test_credentials_are_environment_references(self):
        cmd = h.command('codex', '/bin/codex', 'prompt', 'http://127.0.0.1:1/mcp', '/config')
        self.assertIn('mcp_servers.task-queue.env_http_headers={\"Mo-Upgrade-Authorization\"=\"QUEUE_UPGRADE_BEARER\"}', cmd)
        self.assertIn('QUEUE_TOKEN', ' '.join(cmd))
        self.assertIn('gpt-6.1-sol', cmd)
        self.assertIn('QUEUE_UPGRADE', h.ENTER)
        self.assertEqual(h.SLOTS, ['codex', 'claude', 'codex', 'claude'])

    def test_missing_freeze_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'freeze.json'
            path.write_text(json.dumps({'repository': tmp, 'repair_commit': h.BASE}))
            with self.assertRaisesRegex(ValueError, 'merged repair'):
                h.freeze(path, h.layout())

    def test_codex_uses_http_transport_and_full_access(self):
        cmd = h.command('codex', Path('/bin/codex'), 'p', 'http://x/mcp', Path('/c'))
        self.assertIn('model_provider="responses_http"', cmd)
        self.assertIn('supports_websockets=false', ' '.join(cmd))
        self.assertEqual(cmd[cmd.index('--sandbox') + 1], 'danger-full-access')
        self.assertNotIn('workspace-write', ' '.join(cmd))
        self.assertNotIn('responses_http', ' '.join(h.command('claude', Path('/bin/claude'), 'p', 'http://x/mcp', Path('/c'))))

    def test_codex_alone_gets_the_characterized_log_filter(self):
        self.assertEqual(h.client_environment('codex', {'A': '1'}),
                         {'A': '1', 'RUST_LOG': 'codex_api=trace,codex_core=debug,codex_http_client=debug'})
        self.assertEqual(h.client_environment('claude', {'A': '1'}), {'A': '1'})

    def test_public_stderr_drops_every_tracing_record(self):
        sse = ('2026-11-09T20:33:31Z TRACE codex_api::sse::responses: SSE event: '
               + json.dumps({'response': {'delta': 'a\u2028b', 'safety_identifier': 'private'}}, ensure_ascii=False))
        text = '\n'.join([sse,
            '2026-11-09T20:33:30Z DEBUG session_loop{thread_id=t}: codex_http_client::client: Request completed set-cookie',
            '2026-11-09T20:33:30Z DEBUG codex_core::session::session: Configuring session',
            '  INFO codex_core::x: indented', 'error: plain client message', ''])
        self.assertEqual(h.public_stderr(text), ['error: plain client message'])

    def test_typed_final_messages(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'stream'
            for client, events, answer in (
                ('codex', [{'type': 'item.completed', 'item': {'type': 'agent_message', 'text': ' 1\n'}}, {'type': 'turn.completed'}], ' 1\n'),
                ('codex', [{'type': 'item.completed', 'item': {'type': 'agent_message', 'text': 'Revision: 1'}}, {'type': 'turn.completed'}], 'Revision: 1'),
                ('codex', [{'type': 'item.completed', 'item': {'type': 'agent_message', 'text': '1'}}, {'type': 'item.completed', 'item': {'type': 'command_execution'}}, {'type': 'turn.completed'}], None),
                ('claude', [{'type': 'result', 'subtype': 'error_during_execution', 'result': '1'}], None),
                ('codex', [{'type': 'item.started', 'item': {'type': 'agent_message', 'text': '1'}}], None),
                ('claude', [{'type': 'assistant', 'message': {'content': [{'type': 'text', 'text': '1'}]}}, {'type': 'result', 'subtype': 'success', 'result': '1'}], '1'),
                ('claude', [{'type': 'assistant', 'message': {'content': [{'type': 'text', 'text': '1'}]}}], None),
            ):
                path.write_text(''.join(json.dumps(e) + '\n' for e in events))
                self.assertEqual(h.final_message(client, path), answer)

    def test_concurrent_collection_cannot_reset_account(self):
        import fcntl
        from types import SimpleNamespace
        with tempfile.TemporaryDirectory() as tmp:
            state = Path(tmp)
            with (state / 'collector.lock').open('a') as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                with patch.object(h.p4, 'reset') as reset:
                    with self.assertRaises(BlockingIOError):
                        h.collect(SimpleNamespace(state=state), 'absent', state / 'out', 1)
                    reset.assert_not_called()
            self.assertFalse((state / 'out').exists())

    def test_scored_collection_requires_freeze(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            spec = root / 'spec.json'
            spec.write_text(json.dumps(dict(source=tmp, context=tmp, data=tmp,
                                           freeze=str(root / 'missing-freeze.json'))))
            from types import SimpleNamespace
            with patch.object(h.p4, 'check_private'), patch.object(h, 'check'):
                with self.assertRaises(FileNotFoundError) as error:
                    h.collect(SimpleNamespace(state=root), spec, root / 'out', 1)
                self.assertEqual(error.exception.filename, str(root / 'missing-freeze.json'))

    def test_launch_failure_resets_without_unverified_source_copy(self):
        import contextlib
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            l = h.p4.Layout(user='unused', install=root / 'install', private=root,
                            state=root / 'state')
            l.session.mkdir(parents=True)
            l.state.mkdir()
            source, data = root / 'source', root / 'data'
            source.mkdir()
            data.mkdir()
            (source / 'main.mo').write_text('module Main\n')
            expected = root / 'expected.json'
            expected.write_text('{"revision":"1"}')
            spec = root / 'spec.json'
            spec.write_text(json.dumps({k: str(v) for k, v in dict(
                source=source, context=source, data=data, expected=expected,
                experiment=root / 'experiment', prompt=root / 'prompt').items()}))
            with contextlib.ExitStack() as stack:
                for name in ('check_private', 'place_read_only', 'sudo', 'end_all'):
                    stack.enter_context(patch.object(h.p4, name))
                reset = stack.enter_context(patch.object(h.p4, 'reset'))
                compiles = stack.enter_context(patch.object(h.p4, 'as_session_user', return_value=(0, '')))
                capture = stack.enter_context(patch.object(h.p4, 'collect_work', return_value=0))
                stack.enter_context(patch.object(h.subprocess, 'Popen', side_effect=OSError('launch failed')))
                self.assertEqual(h.collect(l, spec, root / 'out', 1, development=True), 1)
            record = json.loads((root / 'out/manifest.json').read_text())
            self.assertFalse(record['correct'])
            self.assertTrue(record['fixture_failure'])
            for dimension in ('task_state', 'activation_identity', 'final_answer', 'source_tests'):
                self.assertIs(record['dimensions'][dimension], False)
            self.assertIn('OSError', record['failures'])
            compiles.assert_called_once()
            self.assertEqual(reset.call_count, 2)
            capture.assert_not_called()  # Cleanup was not verified; do not copy mutable files.

    def test_final_limit_fixed(self):
        with self.assertRaises(ValueError):
            h.collect(h.layout(), 'absent', 'absent', 1, limit=1)

if __name__ == '__main__':
    unittest.main()
