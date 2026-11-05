#!/usr/bin/env python3
"""Native-client live-view smoke; forwarding proxy records routing, never synthesizes tools."""
import http.client
import http.server
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'preflight'))
import client_probe as preflight


def forwarding(upstream, recorder):
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            body = self.rfile.read(int(self.headers.get('Content-Length', '0')))
            message = json.loads(body)
            tool = message.get('params', {}).get('name') if message.get('method') == 'tools/call' else None
            recorder.write({'direction': 'request', 'rpc': message.get('method'), 'tool': tool,
                            'authorized': self.headers.get('Authorization') == f'Bearer {preflight.TOKEN}'})
            headers = {k: v for k, v in self.headers.items()
                       if k.lower() not in ('host', 'connection', 'content-length')}
            connection = http.client.HTTPConnection('127.0.0.1', upstream, timeout=60)
            connection.request('POST', '/mcp', body, headers)
            response = connection.getresponse()
            data = response.read()
            result = json.loads(data).get('result', {}) if data else {}
            recorder.write({'direction': 'response', 'status': response.status, 'answered_by': 'server',
                            'tool': tool, 'listed': [t['name'] for t in result['tools']] if 'tools' in result else None,
                            'is_error': result.get('isError'), 'structured': result.get('structuredContent')})
            self.send_response(response.status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            connection.close()
    return Handler


preflight.proxy_handler = forwarding
original_run = preflight.run


def pinned_run(command, workdir, out):
    if command[0] == 'codex':
        command[2:2] = ['--model', 'gpt-6.1-sol', '-c', 'model_reasoning_effort="xhigh"']
    else:
        command[1:1] = ['--model', 'claude-opus-5-5']
    return original_run(command, workdir, out)


preflight.run = pinned_run
if __name__ == '__main__':
    sys.exit(preflight.main(sys.argv[1:]))
