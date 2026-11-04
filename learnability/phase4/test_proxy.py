#!/usr/bin/env python3
"""Tests for learnability/phase4/proxy.py: the recording proxy in front of a real `mo serve` of
the synthetic variant in testdata/, and the record match and host checks on made-up streams.

Run: python3 learnability/phase4/test_proxy.py build/gcc/debug/mo [more mo binaries]
"""

import http.client
import json
import os
import pathlib
import secrets
import shlex
import signal
import subprocess
import sys
import tempfile
import threading
import unittest
import urllib.parse

here = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(here))
import fixture  # noqa: E402
import proxy  # noqa: E402

SYNTHETIC = here / 'testdata' / 'variants' / 'synthetic'
PROBE_LOG = here / 'preflight' / 'results' / 'clients' / 'traffic.jsonl'
MO = None  # set per binary by main


def rpc(url, message, token=None, chunked=False):
    """Sends one JSON-RPC message to url; returns the status and the parsed reply."""
    parts = urllib.parse.urlsplit(url)
    body = json.dumps(message).encode()
    headers = {'Content-Type': 'application/json', 'MCP-Protocol-Version': fixture.PROTOCOL,
               'Accept': 'application/json, text/event-stream'}
    if token:
        headers['Authorization'] = f'Bearer {token}'
    connection = http.client.HTTPConnection('127.0.0.1', parts.port, timeout=60)
    try:
        if chunked:
            connection.putrequest('POST', parts.path)
            for k, v in {**headers, 'Transfer-Encoding': 'chunked'}.items():
                connection.putheader(k, v)
            connection.endheaders()
            for at in range(0, len(body), 7):
                piece = body[at:at + 7]
                connection.send(f'{len(piece):x}\r\n'.encode() + piece + b'\r\n')
            connection.send(b'0\r\n\r\n')
        else:
            connection.request('POST', parts.path, body, headers)
        response = connection.getresponse()
        data = response.read()
    finally:
        connection.close()
    if response.getheader('Content-Type', '').startswith('application/json'):
        return response.status, json.loads(data)
    return response.status, data


def call(ident, tool, **arguments):
    return {'jsonrpc': '2.0', 'id': ident, 'method': 'tools/call',
            'params': {'name': tool, 'arguments': arguments}}


class Live(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.folder.name)
        (self.root / 'data').mkdir()
        self.token = secrets.token_urlsafe(32)
        self.service = fixture.Service(MO, SYNTHETIC / 'main.mo', self.root / 'data', self.token,
                                       self.root / 'stderr')
        self.log = self.root / 'proxy.jsonl'
        self.proxy = proxy.Proxy(self.service.url, self.log, self.token)

    def tearDown(self):
        self.proxy.stop()
        self.service.stop()
        self.folder.cleanup()

    def logged(self):
        return proxy.entries(self.log)

    def assert_no_token(self):
        text = self.log.read_text()
        for at in range(len(self.token) - 15):
            self.assertNotIn(self.token[at:at + 16], text)

    def test_a_tool_call_is_recorded_with_both_bodies_and_no_token(self):
        rpc(self.proxy.url, {'jsonrpc': '2.0', 'id': 0, 'method': 'initialize', 'params': {
            'protocolVersion': fixture.PROTOCOL, 'capabilities': {},
            'clientInfo': {'name': 'test', 'version': '1'}}}, self.token)
        rpc(self.proxy.url, {'jsonrpc': '2.0', 'id': 1, 'method': 'tools/list'}, self.token)
        status, reply = rpc(self.proxy.url, call(2, 'submit', payload='brief one'), self.token)
        self.assertEqual((status, reply['result']['isError']), (200, False))
        request, response = self.logged()[-2:]
        self.assertEqual((request['direction'], request['rpc'], request['id'], request['tool'],
                          request['authorized']), ('request', 'tools/call', 2, 'submit', True))
        self.assertEqual(request['body']['params']['arguments'], {'payload': 'brief one'})
        self.assertEqual((response['direction'], response['request'], response['status'],
                          response['tool'], response['is_error']),
                         ('response', request['seq'], 200, 'submit', False))
        self.assertEqual(response['body'], reply)
        self.assertGreaterEqual(response['elapsed'], 0)
        listed = [e['listed'] for e in self.logged() if e.get('listed')]
        self.assertEqual(listed, [fixture.TOOLS])
        self.assert_no_token()

    def test_the_probes_summary_fields_are_in_the_new_log(self):
        rpc(self.proxy.url, {'jsonrpc': '2.0', 'id': 1, 'method': 'tools/list'}, self.token)
        rpc(self.proxy.url, call(2, 'stats'), self.token)
        probe = [json.loads(line) for line in PROBE_LOG.read_text().splitlines()]
        new = self.logged()
        for direction in ('request', 'response'):
            old_keys = set().union(*(e.keys() for e in probe if e['direction'] == direction))
            new_keys = set().union(*(e.keys() for e in new if e['direction'] == direction))
            # The probe answered its stand-in tools itself; every response here is the
            # service's, so who answered is not recorded.
            self.assertEqual(old_keys - new_keys - {'answered_by'}, set(), direction)

    def test_a_request_without_the_token_is_forwarded_refused_and_recorded(self):
        status, _ = rpc(self.proxy.url, call(1, 'stats'))
        self.assertEqual(status, 401)
        request, response = self.logged()
        self.assertFalse(request['authorized'])
        self.assertEqual(response['status'], 401)
        status, _ = rpc(self.proxy.url, call(2, 'stats'), 'not-the-token')
        self.assertEqual(status, 401)
        self.assertFalse(self.logged()[-2]['authorized'])

    def test_a_chunked_request_body_is_forwarded_whole(self):
        payload = 'a payload long enough to arrive in many small chunks ' * 4
        status, reply = rpc(self.proxy.url, call(5, 'submit', payload=payload), self.token,
                            chunked=True)
        self.assertEqual((status, reply['result']['isError']), (200, False))
        _, page = rpc(self.proxy.url, call(6, 'list', filter={'case': 'AllJobs'}, after='0',
                                           limit='1'), self.token)
        self.assertEqual(page['result']['structuredContent']['value']['jobs'][0]['payload'],
                         payload)
        self.assertEqual(self.logged()[0]['body']['params']['arguments']['payload'], payload)

    def test_concurrent_calls_are_each_recorded_with_their_own_numbers(self):
        threads = [threading.Thread(target=rpc, args=(self.proxy.url, call(i, 'stats'),
                                                      self.token)) for i in (1, 2, 3)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        logged = self.logged()
        self.assertEqual(len(logged), 6)
        self.assertEqual([e['seq'] for e in logged], list(range(1, 7)))
        requests = {e['seq']: e for e in logged if e['direction'] == 'request'}
        responses = [e for e in logged if e['direction'] == 'response']
        self.assertEqual(sorted(e['id'] for e in requests.values()), [1, 2, 3])
        self.assertEqual(sorted(requests[e['request']]['id'] for e in responses), [1, 2, 3])
        self.assertEqual(len({e['connection'] for e in requests.values()}), 3)

    def test_a_token_in_a_body_is_written_as_a_placeholder(self):
        rpc(self.proxy.url, call(1, 'submit', payload=f'see {self.token}'), self.token)
        request = self.logged()[0]
        self.assertTrue(request['redacted'])
        self.assertEqual(request['body']['params']['arguments']['payload'], 'see [token]')
        self.assert_no_token()

    def test_a_service_that_is_gone_is_recorded_as_502(self):
        self.service.stop()
        status, _ = rpc(self.proxy.url, call(1, 'stats'), self.token)
        self.assertEqual(status, 502)
        self.assertEqual(self.logged()[-1]['status'], 502)
        self.service = fixture.Service(MO, SYNTHETIC / 'main.mo', self.root / 'data',
                                       self.token, self.root / 'stderr')

    def test_the_command_serves_until_it_is_stopped(self):
        log = self.root / 'command.jsonl'
        process = subprocess.Popen(
            [sys.executable, str(here / 'proxy.py'), 'serve', self.service.url, str(log),
             'PROXY_TOKEN'], stdout=subprocess.PIPE, text=True,
            env={**os.environ, 'PROXY_TOKEN': self.token})
        try:
            line = process.stdout.readline()
            self.assertTrue(line.startswith('listening on http://127.0.0.1:'), line)
            status, _ = rpc(line.split()[-1], call(1, 'stats'), self.token)
            self.assertEqual(status, 200)
        finally:
            process.send_signal(signal.SIGTERM)
            self.assertEqual(process.wait(timeout=30), 0)
            process.stdout.close()
        self.assertEqual([e['direction'] for e in proxy.entries(log)], ['request', 'response'])
        self.assertTrue(proxy.entries(log)[0]['authorized'])


def write_log(folder, calls, unanswered=()):
    """A proxy log with initialize, tools/list and a notification, then each (id, tool,
    arguments) as a tools/call request and, unless its id is unanswered, a response."""
    lines, seq = [], 0

    def add(entry):
        nonlocal seq
        seq += 1
        lines.append(json.dumps({'seq': seq, **entry}))
        return seq

    for ident, method in ((0, 'initialize'), (None, 'notifications/initialized'),
                          (1, 'tools/list')):
        request = add({'direction': 'request', 'rpc': method, 'id': ident, 'body': {}})
        add({'direction': 'response', 'request': request, 'status': 200})
    for ident, tool, arguments in calls:
        request = add({'direction': 'request', 'rpc': 'tools/call', 'id': ident, 'tool': tool,
                       'body': {'params': {'name': tool, 'arguments': arguments}}})
        if ident not in unanswered:
            add({'direction': 'response', 'request': request, 'status': 200})
    path = pathlib.Path(folder) / f'log-{secrets.token_hex(4)}.jsonl'
    path.write_text('\n'.join(lines) + '\n')
    return path


def claude_stream(folder, calls, unanswered=(), commands=()):
    """A Claude Code stream: each (tool, arguments) as a tool_use of the task-queue server with a
    result unless its index is unanswered, then each shell command."""
    events = [{'type': 'system', 'subtype': 'init', 'tools': []}]
    for i, (tool, arguments) in enumerate(calls):
        events.append({'type': 'assistant', 'message': {'content': [
            {'type': 'tool_use', 'id': f'toolu_{i}', 'name': f'mcp__task-queue__{tool}',
             'input': arguments}]}})
        if i not in unanswered:
            events.append({'type': 'user', 'message': {'content': [
                {'type': 'tool_result', 'tool_use_id': f'toolu_{i}', 'content': '{}'}]}})
    for i, command in enumerate(commands):
        events.append({'type': 'assistant', 'message': {'content': [
            {'type': 'tool_use', 'id': f'toolu_b{i}', 'name': 'Bash',
             'input': {'command': command}}]}})
    path = pathlib.Path(folder) / f'claude-{secrets.token_hex(4)}.jsonl'
    path.write_text('\n'.join(json.dumps(e) for e in events) + '\n')
    return path


def codex_stream(folder, calls, unanswered=(), commands=()):
    events = [{'type': 'thread.started'}]
    for i, (tool, arguments) in enumerate(calls):
        item = {'id': f'item_{i}', 'type': 'mcp_tool_call', 'server': 'task-queue',
                'tool': tool, 'arguments': arguments, 'status': 'in_progress'}
        events.append({'type': 'item.started', 'item': item})
        if i not in unanswered:
            events.append({'type': 'item.completed', 'item': {**item, 'status': 'completed'}})
    for i, command in enumerate(commands):
        events.append({'type': 'item.completed', 'item': {
            'id': f'item_c{i}', 'type': 'command_execution', 'command': command}})
    path = pathlib.Path(folder) / f'codex-{secrets.token_hex(4)}.jsonl'
    path.write_text('\n'.join(json.dumps(e) for e in events) + '\n')
    return path


class Match(unittest.TestCase):
    CALLS = [('submit', {'payload': 'a'}), ('stats', {}), ('submit', {'payload': 'b'})]

    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.root = self.folder.name

    def tearDown(self):
        self.folder.cleanup()

    def logged(self, calls, **kw):
        return write_log(self.root, [(10 + i, t, a) for i, (t, a) in enumerate(calls)], **kw)

    def test_records_that_agree_match_for_both_clients(self):
        log = self.logged(self.CALLS)
        for make, client in ((claude_stream, 'claude'), (codex_stream, 'codex')):
            result = proxy.match(client, make(self.root, self.CALLS), log)
            self.assertEqual((result['match'], result['pairs']), (True, 3), client)

    def test_calls_made_in_another_order_still_pair(self):
        log = self.logged(self.CALLS)
        swapped = [self.CALLS[2], self.CALLS[1], self.CALLS[0]]
        self.assertTrue(proxy.match('claude', claude_stream(self.root, swapped), log)['match'])

    def test_a_call_whose_arguments_differ_fails_on_both_sides(self):
        log = self.logged(self.CALLS)
        changed = [self.CALLS[0], self.CALLS[1], ('submit', {'payload': 'c'})]
        for make, client, ident in ((claude_stream, 'claude', 'toolu_2'),
                                    (codex_stream, 'codex', 'item_2')):
            result = proxy.match(client, make(self.root, changed), log)
            self.assertFalse(result['match'])
            self.assertEqual((result['log_only'], result['stream_only']),
                             ([{'log_id': 12, 'tool': 'submit'}],
                              [{'stream_id': ident, 'tool': 'submit'}]))

    def test_an_extra_logged_call_fails_naming_its_id(self):
        log = self.logged(self.CALLS + [('stats', {})])
        for make, client in ((claude_stream, 'claude'), (codex_stream, 'codex')):
            result = proxy.match(client, make(self.root, self.CALLS), log)
            self.assertFalse(result['match'])
            self.assertEqual(result['log_only'], [{'log_id': 13, 'tool': 'stats'}])
            self.assertEqual(result['stream_only'], [])

    def test_an_extra_stream_call_fails_naming_it(self):
        log = self.logged(self.CALLS)
        for make, client, ident in ((claude_stream, 'claude', 'toolu_3'),
                                    (codex_stream, 'codex', 'item_3')):
            result = proxy.match(client, make(self.root, self.CALLS + [('lease', {})]), log)
            self.assertFalse(result['match'])
            self.assertEqual(result['stream_only'], [{'stream_id': ident, 'tool': 'lease'}])

    def test_a_call_in_flight_at_the_end_is_exempt(self):
        log = self.logged(self.CALLS + [('lease', {})], unanswered=(13,))
        result = proxy.match('claude', claude_stream(self.root, self.CALLS), log)
        self.assertEqual((result['match'], result['exempt']),
                         (True, [{'log_id': 13, 'tool': 'lease'}]))
        stream = codex_stream(self.root, self.CALLS + [('lease', {})], unanswered=(3,))
        result = proxy.match('codex', stream, self.logged(self.CALLS))
        self.assertEqual((result['match'], result['exempt']),
                         (True, [{'stream_id': 'item_3', 'tool': 'lease'}]))

    def test_initialize_list_and_other_servers_do_not_count(self):
        log = self.logged([])
        stream = claude_stream(self.root, [])
        with open(stream, 'a') as file:
            file.write(json.dumps({'type': 'assistant', 'message': {'content': [
                {'type': 'tool_use', 'id': 'x', 'name': 'mcp__other__stats', 'input': {}},
                {'type': 'tool_use', 'id': 'y', 'name': 'Read', 'input': {}}]}}) + '\n')
        self.assertEqual(proxy.match('claude', stream, log),
                         {'match': True, 'pairs': 0, 'log_only': [], 'stream_only': [],
                          'exempt': []})

    def test_the_command_exits_by_the_result(self):
        log = self.logged(self.CALLS)
        ok = subprocess.run([sys.executable, str(here / 'proxy.py'), 'match', 'codex',
                             str(codex_stream(self.root, self.CALLS)), str(log)],
                            capture_output=True, text=True, timeout=60)
        self.assertEqual(ok.returncode, 0, ok.stderr)
        self.assertTrue(json.loads(ok.stdout)['match'])
        bad = subprocess.run([sys.executable, str(here / 'proxy.py'), 'match', 'codex',
                              str(codex_stream(self.root, self.CALLS[:1])), str(log)],
                             capture_output=True, text=True, timeout=60)
        self.assertEqual(bad.returncode, 1)


class Hosts(unittest.TestCase):
    LOCAL = ['mo check queue-tests.mo', 'cat ../service.log | tail -5',
             'curl -s http://127.0.0.1:8123/mcp -H "Authorization: Bearer $QUEUE_TOKEN"',
             'curl -o out.json localhost:8123/mcp', 'wget -O page.html http://[::1]:80/',
             'python3 -c "import urllib.request; urllib.request.urlopen(\'http://localhost:1/\')"',
             'git log -1 --format=%ae', 'ls ~/start-data && sqlite3 service.sqlite .tables',
             'python3 -c "import os.path; print(open(\'main.mo\').read())"',
             'grep -n "service.log" README.md; mo test queue-tests.mo']
    FOREIGN = [('curl https://github.com/robertguss/mo', 'github.com'),
               ('wget example.org/x.tgz', 'example.org'),
               ('ssh robert@build.example.net uptime', 'build.example.net'),
               ('nc 10.0.0.7 22', '10.0.0.7'),
               ('python3 -c "import urllib.request as u; u.urlopen(\'https://pypi.org/\')"',
                'pypi.org'),
               ('curl -H "x: y" raw.githubusercontent.com/a/b', 'raw.githubusercontent.com'),
               ('python3 -c "import socket; socket.create_connection((\'example.net\', 443))"',
                'example.net'),
               ('cd /tmp&&curl files.example.io', 'files.example.io'),
               ('x=$(wget -qO- example.dev/run)', 'example.dev'),
               ('python3 fetch.py mirror.example.org', 'mirror.example.org')]

    def test_loopback_commands_name_no_host_and_others_are_named(self):
        with tempfile.TemporaryDirectory() as root:
            commands = self.LOCAL + [c for c, _ in self.FOREIGN]
            wrapped = [f'/bin/bash -lc {shlex.quote(c)}' for c in commands]
            for make, client, given in ((claude_stream, 'claude', commands),
                                        (codex_stream, 'codex', commands),
                                        (codex_stream, 'codex', wrapped)):
                found = proxy.hosts(client, make(root, [], commands=given))
                self.assertEqual([f['host'] for f in found], [h for _, h in self.FOREIGN],
                                 client)
                self.assertEqual(found[0]['command'], given[len(self.LOCAL)])

    def test_web_tools_are_named(self):
        with tempfile.TemporaryDirectory() as root:
            path = pathlib.Path(root) / 'claude.jsonl'
            path.write_text(json.dumps({'type': 'assistant', 'message': {'content': [
                {'type': 'tool_use', 'id': 'a', 'name': 'WebFetch',
                 'input': {'url': 'https://docs.example.com/mo'}},
                {'type': 'tool_use', 'id': 'b', 'name': 'WebSearch',
                 'input': {'query': 'mo language'}}]}}) + '\n')
            self.assertEqual(proxy.hosts('claude', path), [
                {'host': 'docs.example.com', 'web fetch': 'https://docs.example.com/mo'},
                {'host': 'web search', 'web search': 'mo language'}])


def main():
    binaries = sys.argv[1:]
    if not binaries:
        print(__doc__.split('\n\n')[1], file=sys.stderr)
        return 2
    global MO
    ok = True
    for binary in binaries:
        MO = os.path.abspath(binary)
        print(f'phase 4 proxy with {binary}', file=sys.stderr)
        suite = unittest.TestSuite()
        for case in (Live, Match, Hosts):
            suite.addTests(unittest.defaultTestLoader.loadTestsFromTestCase(case))
        ok = unittest.TextTestRunner(verbosity=1).run(suite).wasSuccessful() and ok
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
