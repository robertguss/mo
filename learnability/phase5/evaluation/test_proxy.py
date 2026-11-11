import os
import base64
import contextlib
import http.client
import http.server
import io
import json
import pathlib
import tempfile
import threading
import unittest
import proxy


class Tests(unittest.TestCase):
    def test_wrapped_commands(self):
        for command in ("mo push main.mo", "/bin/bash -lc 'mo push main.mo'",
                        ['/bin/bash', '-lc', 'mo push main.mo'],
                        "'/opt/mo-phase5/bin/mo' push main.mo", "cd /tmp && mo push main.mo"):
            self.assertTrue(proxy.is_push(command), command)
        for command in ('echo no-push', 'echo mo push main.mo', "echo 'mo push main.mo'", 'mo check main.mo', "'broken"):
            self.assertFalse(proxy.is_push(command), command)

    def test_forward_and_redact(self):
        tokens = ['service-secret-0123456789', 'upgrade-secret-9876543210']
        seen = []
        class Upstream(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass
            def do_POST(self):
                body = self.rfile.read(int(self.headers['Content-Length']))
                seen.append((self.headers['Authorization'],
                             self.headers['Mo-Upgrade-Authorization'], body))
                self.send_response(500)
                self.end_headers()
                self.wfile.write(body)
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Upstream)
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory:
                log = pathlib.Path(directory) / 'proxy.jsonl'
                stdout, stderr = io.StringIO(), io.StringIO()
                with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                    recorder = proxy.Proxy(f'http://127.0.0.1:{server.server_port}/mcp', log, *tokens)
                    try:
                        for token in tokens:
                            for value in [token, base64.b64encode(token.encode()).decode(),
                                          token.encode().hex(), ''.join('%%%02x' % b for b in token.encode())]:
                                body = json.dumps({'error': value}).encode()
                                conn = http.client.HTTPConnection('127.0.0.1', recorder.server.server_port)
                                conn.request('POST', '/mcp', body, {
                                    'Authorization': 'Bearer ' + tokens[0],
                                    'Mo-Upgrade-Authorization': 'Bearer ' + tokens[1]})
                                response = conn.getresponse()
                                self.assertEqual(response.status, 500)
                                self.assertEqual(response.read(), body)
                                conn.close()
                    finally:
                        recorder.stop()
                self.assertTrue(all(a == 'Bearer ' + tokens[0] and b == 'Bearer ' + tokens[1]
                                    for a, b, _ in seen))
                self.assertEqual(stdout.getvalue() + stderr.getvalue(), '')
                table = proxy.scan.patterns(list(enumerate((t.encode() for t in tokens), 1)))
                self.assertEqual(proxy.scan.scan_file(log, table, max(map(len, table)) - 1), {})
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_synthesized_unreachable_reply_is_recorded_exactly(self):
        import hashlib, socket
        tokens = ['service-secret-0123456789', 'upgrade-secret-9876543210']
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0)); port = probe.getsockname()[1]
        with tempfile.TemporaryDirectory() as directory:
            log = pathlib.Path(directory) / 'proxy.jsonl'
            with contextlib.redirect_stderr(io.StringIO()):
                recorder = proxy.Proxy(f'http://127.0.0.1:{port}/mcp', log, *tokens)
                try:
                    conn = http.client.HTTPConnection('127.0.0.1', recorder.server.server_port)
                    conn.request('GET', '/mcp', headers={'Authorization': 'Bearer ' + tokens[0]})
                    response = conn.getresponse(); sent = response.read(); conn.close()
                finally:
                    recorder.stop()
            self.assertEqual(response.status, 502)
            reply = [r for r in proxy.base.entries(log) if r['direction'] == 'response'][0]
            self.assertEqual(reply['transport']['body_byte_count'], len(sent))
            self.assertEqual(reply['transport']['body_sha256'], hashlib.sha256(sent).hexdigest())

    def test_command_origin(self):
        with tempfile.TemporaryDirectory() as directory:
            stream, log = [pathlib.Path(directory) / name for name in ('stream', 'log')]
            stream.write_text('')
            log.write_text(json.dumps({'seq': 1, 'direction': 'request', 'rpc': 'tools/call',
                                      'id': 1, 'tool': 'mo-upgrade-open', 'body': {'params': {'arguments': {}}}})
                           + '\n' + json.dumps({'direction': 'response', 'request': 1}) + '\n')
            self.assertFalse(proxy.match('codex', stream, log)['match'])
            stream.write_text(json.dumps({'type': 'item.completed', 'item': {
                'type': 'command_execution', 'command': 'mo push main.mo --url URL'}}) + '\n')
            result = proxy.match('codex', stream, log)
            self.assertFalse(result['match'])
            self.assertEqual(result['command_origin'], [])
            log.write_text(log.read_text().splitlines()[0] + '\n')
            unanswered = proxy.match('codex', stream, log)
            self.assertFalse(unanswered['match'])
            self.assertEqual([c['tool'] for c in unanswered['exempt']], ['mo-upgrade-open'])
            self.assertEqual(unanswered['command_origin'], [])


    def test_upload_routes_follow_stream_twins(self):
        with tempfile.TemporaryDirectory() as directory:
            stream, log = [pathlib.Path(directory) / name for name in ('stream', 'log')]
            rows = []
            for seq, tool in ((1, 'mo-upgrade-open'), (3, 'mo-upgrade-commit'), (5, 'revision')):
                rows += [{'seq': seq, 'direction': 'request', 'rpc': 'tools/call', 'id': seq, 'tool': tool,
                          'body': {'params': {'arguments': {'n': seq}}}},
                         {'seq': seq + 1, 'direction': 'response', 'request': seq}]
            log.write_text(''.join(json.dumps(r) + '\n' for r in rows))
            item = {'id': 'a', 'type': 'mcp_tool_call', 'server': 'task-queue', 'tool': 'mo-upgrade-open',
                    'arguments': {'n': 1}}
            stream.write_text(''.join(json.dumps({'type': kind, 'item': item}) + '\n'
                                      for kind in ('item.started', 'item.completed')))
            result = proxy.match('codex', stream, log)
            self.assertEqual(result['upload_routes'], dict(native=1, shell=1))
            self.assertEqual([c['tool'] for c in result['log_only']], ['mo-upgrade-commit', 'revision'])


class RealPush(unittest.TestCase):
    def test_native_upgrade_requires_bearer_scheme(self):
        import fixture
        import secrets
        import urllib.parse
        mo = pathlib.Path(os.environ.get('MO_EVAL_BINARY', str(fixture.ROOT / 'build/eval-controls/debug/mo')))
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            token, upgrade = secrets.token_hex(24), secrets.token_hex(24)
            source = fixture.sources(root / 'source', 'unchanged')
            service = fixture.Service(mo, root / 'data', token, upgrade,
                                      root / 'service.log', source / 'main.mo')
            recorder = proxy.Proxy(service.url, root / 'proxy.jsonl', token, upgrade)
            try:
                port = urllib.parse.urlsplit(recorder.url).port
                request = {'jsonrpc': '2.0', 'id': 1, 'method': 'tools/call', 'params': {
                    'name': 'mo-upgrade-open', 'arguments': {
                        'base': '0' * 64, 'candidate': '1' * 64, 'total': 1}}}
                for header, expected in ((upgrade, 'E1414'), ('Bearer ' + upgrade, 'E1416')):
                    conn = http.client.HTTPConnection('127.0.0.1', port, timeout=30)
                    try:
                        conn.request('POST', '/mcp', json.dumps(request), {
                            'Authorization': 'Bearer ' + token,
                            'Mo-Upgrade-Authorization': header,
                            'Content-Type': 'application/json', 'MCP-Protocol-Version': '2025-11-25'})
                        response = conn.getresponse()
                        result = json.loads(response.read())['result']
                        self.assertEqual(response.status, 200)
                        self.assertTrue(result['isError'])
                        self.assertEqual(result['structuredContent']['code'], expected)
                    finally:
                        conn.close()
            finally:
                recorder.stop()
                service.stop()

    def test_real_push_records_the_accepted_commit_on_the_shell_route(self):
        import fixture
        import grade
        import os
        import secrets
        import subprocess
        mo = pathlib.Path(os.environ.get('MO_EVAL_BINARY', str(fixture.ROOT / 'build/eval-controls/debug/mo')))
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            token, upgrade = secrets.token_hex(24), secrets.token_hex(24)
            old = fixture.sources(root / 'old', 'unchanged')
            candidate = fixture.sources(root / 'candidate')
            service = fixture.Service(mo, root / 'data', token, upgrade,
                                      root / 'service.log', old / 'main.mo')
            record = proxy.Proxy(service.url, root / 'proxy.jsonl', token, upgrade)
            try:
                checked = subprocess.run([str(mo), 'check', str(candidate / 'main.mo'),
                    '--upgrade-from', str(old / 'main.mo'), '--migrate', 'migrate', '--json'],
                    capture_output=True, text=True, timeout=120, check=True)
                identity = json.loads(checked.stdout)
                command = [str(mo), 'push', str(candidate / 'main.mo'), '--url', record.url,
                    '--base', identity['base_source'], '--migrate', 'migrate',
                    '--token-env', 'QUEUE_TOKEN', '--upgrade-token-env', 'QUEUE_UPGRADE', '--json']
                result = subprocess.run(command, capture_output=True, text=True, timeout=180,
                    env={**os.environ, 'QUEUE_TOKEN': token, 'QUEUE_UPGRADE': upgrade})
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            finally:
                record.stop()
                service.stop()
            stream = root / 'stream.jsonl'
            stream.write_text(json.dumps({'type': 'item.completed', 'item': {
                'type': 'command_execution', 'command': ' '.join(command), 'exit_code': 0}}) + '\n')
            matched = proxy.match('codex', stream, root / 'proxy.jsonl')
            uploads = [r for r in proxy.base.entries(root / 'proxy.jsonl')
                       if r.get('direction') == 'request' and r.get('tool') in proxy.UPLOAD]
            self.assertGreaterEqual(len(uploads), 3)
            self.assertEqual(matched['upload_routes'], dict(native=0, shell=len(uploads)))
            self.assertEqual(len(matched['log_only']), len(uploads))
            accepted = grade.accepted_report(root / 'proxy.jsonl')['report']
            self.assertEqual(accepted['status'], 'accepted')
            self.assertEqual(accepted['candidate_source'], identity['candidate_source'])
            rows = [json.loads(line) for line in (root / 'proxy.jsonl').read_text().splitlines()]
            for row in rows:
                if row.get('direction') == 'response' and row.get('tool') == 'mo-upgrade-commit':
                    row['body']['result']['structuredContent']['status'] = 'refused'
            (root / 'refused.jsonl').write_text(''.join(json.dumps(r) + '\n' for r in rows))
            self.assertIsNone(grade.accepted_report(root / 'refused.jsonl'))


if __name__ == '__main__':
    unittest.main()
