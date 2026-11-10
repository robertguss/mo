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

    def test_agent_dictionary_cannot_supply_origin(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / 'log'
            log.write_text('')
            with self.assertRaisesRegex(ValueError, 'collector Capture required'):
                h.proxy.match('codex', log, log, capture={'qualified': True})

    def test_provider_tls_is_excluded_from_http_reconstruction(self):
        from origin_trace import parse_trace
        from test_origin_trace import line
        trace = line(b'\x16\x03\x01provider tls').replace(':80]', ':443]')
        parsed = parse_trace(trace, evaluation_endpoint='127.0.0.1:80')
        self.assertEqual(parsed['requests'], [])
        self.assertEqual(parsed['responses'], [])

    def test_resumed_vfork_registers_actual_child(self):
        trace = '\n'.join([
            '10 execve("/bin/sh", ["sh"], []) = 0',
            '10 vfork( <unfinished ...>',
            '11 execve("/bin/true", ["true"], []) = 0',
            '10 <... vfork resumed>) = 11',
            '11 +++ exited with 0 +++',
            '10 +++ exited with 0 +++', ''])
        self.assertTrue(h.origin_observer.process_integrity(trace)['process_completeness_verified'])
        truncated = trace.replace('10 <... vfork resumed>) = 11\n', '')
        self.assertFalse(h.origin_observer.process_integrity(truncated)['process_completeness_verified'])

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
        self.assertIs(h.grader.proxy, h.proxy)

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

    def test_codex_uses_traced_http_transport(self):
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

    def test_scored_collection_blocked_before_side_effects(self):
        with self.assertRaisesRegex(ValueError, 'qualification'):
            h.collect(h.layout(), 'absent', 'absent', 1)

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
                capture = stack.enter_context(patch.object(h.p4, 'collect_work', return_value=0))
                stack.enter_context(patch.object(h.subprocess, 'Popen', side_effect=OSError('launch failed')))
                self.assertEqual(h.collect(l, spec, root / 'out', 1, development=True), 1)
            record = json.loads((root / 'out/manifest.json').read_text())
            self.assertFalse(record['correct'])
            self.assertTrue(record['fixture_failure'])
            for dimension in ('task_state', 'activation_identity', 'command_origin', 'final_answer'):
                self.assertIs(record['dimensions'][dimension], False)
            self.assertIn('OSError', record['failures'])
            self.assertEqual(reset.call_count, 2)
            capture.assert_not_called()  # Cleanup was not verified; do not copy mutable files.

    def test_final_limit_fixed(self):
        with self.assertRaises(ValueError):
            h.collect(h.layout(), 'absent', 'absent', 1, limit=1)

if __name__ == '__main__':
    unittest.main()
