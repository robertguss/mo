import json
import os
import unittest
from pathlib import Path

from model_adapter import codex_responses, session_provenance

T = '01a1c204-0b7b-7fb2-8a82-80cf8bd5acca'
BASE = 'https://chatgpt.com/backend-api/codex/'


def req(path='responses', status='200 OK', thread=T, method='POST', failed=False):
    span = f'session_loop{{thread_id={thread}}}:turn{{turn.id=u model=gpt-6.1-sol}}:responses.stream_request{{api.path="/responses"}}'
    if failed:
        return (f'2026-11-09T20:33:30Z DEBUG {span}: codex_http_client::client: Request failed '
                f'method={method} url={BASE}{path} status=None error=connect')
    return (f'2026-11-09T20:33:30Z DEBUG {span}: codex_http_client::client: Request completed '
            f'method={method} url={BASE}{path} status={status} headers={{"x-oai-request-id": "q"}} version=HTTP/2.0')


def sse(kind, rid, model='gpt-6.1-sol', effort='xhigh', usage=None):
    response = {'id': rid, 'model': model, 'reasoning': {'effort': effort},
                'status': 'completed' if kind == 'response.completed' else 'in_progress'}
    if usage is not None:
        response['usage'] = usage
    return '2026-11-09T20:33:31Z TRACE codex_api::sse::responses: SSE event: ' + json.dumps(
        {'type': kind, 'sequence_number': 0, 'response': response})


def usage(i, o):
    return {'input_tokens': i, 'input_tokens_details': {'cached_tokens': 0},
            'output_tokens': o, 'output_tokens_details': {'reasoning_tokens': 0}}


def turn(i, o):
    return {'type': 'turn.completed', 'usage': {'input_tokens': i, 'cached_input_tokens': 0,
                                                 'output_tokens': o, 'reasoning_output_tokens': 0}}


class CodexProvenanceTests(unittest.TestCase):
    def good(self):
        lines = [req(), sse('response.created', 'r1'), sse('response.completed', 'r1', usage=usage(10, 2)),
                 req('analytics-events/events'),
                 req(), sse('response.created', 'r2'), sse('response.completed', 'r2', usage=usage(20, 3))]
        events = [{'type': 'thread.started', 'thread_id': T}, {'type': 'turn.started'}, turn(30, 5)]
        return lines, events

    def check(self, lines, events):
        return codex_responses(lines, events, 'gpt-6.1-sol', 'xhigh')

    def test_positive_binds_every_attempt(self):
        r = self.check(*self.good())
        self.assertTrue(r.complete, r.problems)
        self.assertEqual(r.responses, (('r1', 'gpt-6.1-sol', 'xhigh', 'response.completed'),
                                       ('r2', 'gpt-6.1-sol', 'xhigh', 'response.completed')))
        self.assertEqual(r.attempts, 2)
        self.assertEqual(r.thread, T)

    def test_requested_configuration_alone_fails(self):
        lines = ['DEBUG codex_core::session::session: Configuring session: model=gpt-6.1-sol']
        r = self.check(lines, self.good()[1])
        self.assertFalse(r.complete)

    def test_model_text_outside_provider_trace_is_ignored(self):
        lines, events = self.good()
        forged = [l.replace('codex_api::sse::responses', 'codex_core::exec') if 'SSE' in l else l for l in lines]
        self.assertIn('attempt without provider response', self.check(forged, events).problems)
        events = events + [{'type': 'item.completed', 'item': {'text': 'model gpt-6.1-sol'}}]
        self.assertFalse(self.check(forged, events).complete)

    def test_substituted_model_or_effort_is_retained_and_fails(self):
        for kw in ({'model': 'gpt-6.1-mini'}, {'effort': 'low'}):
            lines, events = self.good()
            lines[1] = sse('response.created', 'r1', **kw)
            r = self.check(lines, events)
            self.assertFalse(r.complete)
            self.assertIn('r1', [x[0] for x in r.substituted])

    def test_terminal_model_disagreeing_with_created_fails(self):
        lines, events = self.good()
        lines[2] = sse('response.completed', 'r1', model='other', usage=usage(10, 2))
        r = self.check(lines, events)
        self.assertFalse(r.complete)
        self.assertEqual([x[0] for x in r.substituted], ['r1'])

    def test_unanswered_or_unrequested_responses_fail(self):
        lines, events = self.good()
        self.assertFalse(self.check(lines + [req()], events).complete)
        self.assertFalse(self.check(lines[1:], events).complete)

    def test_duplicate_or_terminal_without_created_fails(self):
        lines, events = self.good()
        lines[5] = sse('response.created', 'r1')
        self.assertFalse(self.check(lines, events).complete)
        lines, events = self.good()
        del lines[5]
        self.assertFalse(self.check(lines, events).complete)

    def test_usage_must_reconcile_with_client_turns(self):
        lines, events = self.good()
        events[-1] = turn(31, 5)
        self.assertFalse(self.check(lines, events).complete)
        self.assertFalse(self.check(lines, events[:-1]).complete)

    def test_sub_agent_threads_are_retained_but_session_thread_required(self):
        lines, events = self.good()
        lines[4] = req(thread='child')
        r = self.check(lines, events)
        self.assertTrue(r.complete, r.problems)
        self.assertEqual(r.threads, (T, 'child'))
        lines = [l.replace(T, 'child') for l in lines]
        self.assertFalse(self.check(lines, events).complete)
        self.assertFalse(self.check(*self.good()[:1], events[1:]).complete)

    def test_every_provider_request_is_classified(self):
        unspanned = ('2026-11-09T20:33:30Z DEBUG codex_http_client::client: Request completed '
                     'method=POST url=https://chatgpt.com/backend-api/codex/{} status=200 OK headers={{}}')
        for extra in (unspanned.format('responses/compact'), unspanned.format('wham/tasks'),
                      unspanned.format('memories/trace_summarize')):
            lines, events = self.good()
            self.assertFalse(self.check(lines + [extra], events).complete, extra)
        lines, events = self.good()
        local = unspanned.replace('https://chatgpt.com/backend-api/codex/{}', 'http://127.0.0.1:9/mcp')
        r = self.check(lines + [local, req('models', method='GET')], events)
        self.assertTrue(r.complete, r.problems)
        lines.append(req().replace('https://', 'http://'))
        self.assertFalse(self.check(lines, events).complete)

    def test_failed_attempt_then_retry_is_retained(self):
        lines, events = self.good()
        lines = [req(failed=True), req(status='429 Too Many Requests')] + lines
        r = self.check(lines, events)
        self.assertTrue(r.complete, r.problems)
        self.assertEqual((r.attempts, r.failed_attempts), (4, 2))

    def test_truncated_stream_keeps_created_model(self):
        lines, events = self.good()
        lines = [req(), sse('response.created', 'r0')] + lines
        r = self.check(lines, events)
        self.assertTrue(r.complete, r.problems)
        self.assertIn(('r0', 'gpt-6.1-sol', 'xhigh', None), r.responses)

    def test_unaccounted_transport_or_auxiliary_route_fails(self):
        for extra in (req(status='101 Switching Protocols'), req('memories/trace_summarize'),
                      req('alpha/search'), req('realtime/calls')):
            lines, events = self.good()
            self.assertFalse(self.check(lines + [extra], events).complete, extra)
        lines, events = self.good()
        lines.append(req('responses').replace('https://', 'wss://'))
        self.assertFalse(self.check(lines, events).complete)

    def test_malformed_provider_event_fails(self):
        lines, events = self.good()
        lines[2] = lines[2][:-10]
        self.assertFalse(self.check(lines, events).complete)

    @unittest.skipUnless(os.access(Path.home() / '.mo-phase5-harness', os.R_OK), 'private capture absent')
    def test_retained_pinned_client_captures(self):
        # The first capture predates codex_http_client=debug, so it has no attempt ledger.
        for name, complete in (('http-development-20261109-one', False),
                               ('http-header-development-20261109-one', True)):
            base = Path.home() / '.mo-phase5-harness' / name
            lines = (base / 'codex.stderr').read_text(errors='replace').splitlines()
            events = [json.loads(l) for l in (base / 'codex.jsonl').read_text().splitlines() if l.strip()]
            r = self.check(lines, events)
            self.assertEqual(r.complete, complete, (name, r.problems))
            self.assertEqual(len(r.responses), 2)


class SessionProvenanceTests(unittest.TestCase):
    ROOT = dict(pid=10, tgid=10, exec_path='/opt/bin/codex', exec_argv=['codex', 'exec'])

    def evidence(self, writer=None, stderr_extra=b''):
        lines, events = CodexProvenanceTests.good(None)
        stream = ''.join(json.dumps(e) + '\n' for e in events).encode()
        stderr = ('\n'.join(lines) + '\n').encode()
        sender = dict(self.ROOT, **(writer or {}))
        trace = dict(exec_events=[dict(self.ROOT)],
                     stdout_events=[dict(data=stream.decode(), sender=dict(self.ROOT, pid=11))],
                     stderr_events=[dict(data_hex=stderr.hex(), sender=sender)])
        return dict(trace=trace, stdout=stream, stderr=stderr + stderr_extra,
                    client_path='/opt/bin/codex'), stream

    def test_line_separator_inside_provider_json_still_qualifies(self):
        evidence, stream = self.evidence()
        text = evidence['stderr'].decode().replace('"in_progress"', '"in\u2028progress"', 1)
        evidence['stderr'] = text.encode()
        evidence['trace']['stderr_events'][0]['data_hex'] = evidence['stderr'].hex()
        self.assertTrue(session_provenance('codex', evidence, stream)['qualified'])

    def test_uncapturable_client_write_fails(self):
        evidence, stream = self.evidence()
        evidence['trace']['stderr_events'].append(dict(data_hex=None, sender=dict(self.ROOT)))
        self.assertFalse(session_provenance('codex', evidence, stream)['qualified'])

    def test_attested_codex_channels_qualify(self):
        r = session_provenance('codex', *self.evidence())
        self.assertTrue(r['qualified'], r)
        self.assertEqual(len(r['responses']), 2)

    def test_foreign_writer_or_extra_bytes_fail(self):
        for writer in ({'tgid': 12}, {'exec_path': '/bin/sh'}, {'exec_argv': ['codex', 'other']}):
            r = session_provenance('codex', *self.evidence(writer))
            self.assertFalse(r['qualified'])
            self.assertFalse(r['stderr_attested'])
        r = session_provenance('codex', *self.evidence(stderr_extra=b'forged\n'))
        self.assertFalse(r['qualified'])

    def test_missing_stderr_or_changed_stdout_fail(self):
        evidence, stream = self.evidence()
        evidence['stderr'] = None
        self.assertFalse(session_provenance('codex', evidence, stream)['qualified'])
        evidence, stream = self.evidence()
        self.assertFalse(session_provenance('codex', evidence, stream + b'{}\n')['qualified'])

    def test_claude_envelopes_without_reconciliation_do_not_qualify(self):
        events = [dict(type='system', session_id='s'),
                  dict(type='assistant', session_id='s', request_id='r', message=dict(model='claude-opus-5-5')),
                  dict(type='result', session_id='s', subtype='success', is_error=False,
                       modelUsage={'claude-opus-5-5': {}})]
        stream = ''.join(json.dumps(e) + '\n' for e in events).encode()
        root = dict(self.ROOT, exec_path='/opt/bin/claude')
        evidence = dict(trace=dict(exec_events=[root], stdout_events=[dict(data=stream.decode(), sender=root)],
                                   stderr_events=[]), stdout=stream, stderr=b'', client_path='/opt/bin/claude')
        r = session_provenance('claude', evidence, stream)
        self.assertTrue(r['envelope_complete'])
        self.assertFalse(r['qualified'])

    def test_attested_reconciled_claude_qualifies(self):
        use = {'input_tokens': 1, 'cache_read_input_tokens': 2, 'cache_creation_input_tokens': 3}
        events = [dict(type='system', session_id='s'),
                  dict(type='assistant', session_id='s', request_id='r',
                       message=dict(id='m', model='claude-opus-5-5', usage=use)),
                  dict(type='result', session_id='s', subtype='success', is_error=False,
                       modelUsage={'claude-opus-5-5': dict(inputTokens=1, cacheReadInputTokens=2,
                                                           cacheCreationInputTokens=3)})]
        stream = ''.join(json.dumps(e) + '\n' for e in events).encode()
        root = dict(self.ROOT, exec_path='/opt/bin/claude')
        evidence = dict(trace=dict(exec_events=[root], stdout_events=[dict(data=stream.decode(), sender=root)],
                                   stderr_events=[]), stdout=stream, stderr=b'', client_path='/opt/bin/claude')
        self.assertTrue(session_provenance('claude', evidence, stream)['qualified'])
        evidence['trace']['stdout_events'][0]['sender'] = dict(root, tgid=99)
        self.assertFalse(session_provenance('claude', evidence, stream)['qualified'])


if __name__ == '__main__':
    unittest.main()
