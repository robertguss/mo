#!/usr/bin/env python3
"""How native MCP clients name, list and route operator tools beside author tools.

  python3 learnability/phase4/preflight/client_probe.py MO OUT [codex] [codex-approve] [claude]

Starts `mo serve` on clients/main.mo, whose author tools are `mo_state` and `count`, and puts a
recording proxy in front of it. The proxy adds two operator tools to the catalog, `mo-state` and
`mo-overview`, answers calls to them itself, and forwards everything else unchanged, so the
author tools still go through the real server. It records every request and response in order
in OUT/traffic.jsonl. Then it runs each named client once, non-interactively, against the
proxy, asking it to call the three tools by wire name and report the names it sees. Each
client's own event stream is saved as OUT/CLIENT.jsonl; OUT/summary.json joins them: the wire
names listed, the model-visible names each client reported, and which wire tool each call
reached. The token is a fixed canary; the clients read it from the environment.
"""

import http.client
import http.server
import json
import os
import pathlib
import select
import signal
import subprocess
import sys
import tempfile
import threading
import time

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent.parent
TOKEN = 'Phase4ClientProbeCanary'
OPERATOR_TOOLS = [
    {'name': 'mo-state', 'description': 'Operator view: the service state (probe stand-in).',
     'inputSchema': {'type': 'object', 'properties': {}, 'additionalProperties': False},
     'annotations': {'readOnlyHint': True, 'idempotentHint': True}},
    {'name': 'mo-overview', 'description': 'Operator view: an overview (probe stand-in).',
     'inputSchema': {'type': 'object', 'properties': {}, 'additionalProperties': False},
     'annotations': {'readOnlyHint': True, 'idempotentHint': True}},
]
PROMPT = ('An MCP server named probe is connected. Use only its tools and no other tool. Call '
          'each of these three tools exactly once, in this order, with no arguments: the tool '
          'whose server-side name is mo-state, then the tool whose server-side name is '
          'mo_state, then the tool whose server-side name is mo-overview. Then answer with two '
          'lists: every tool name from that server exactly as it appears to you, and the text '
          'each of the three calls returned.')


class Recorder:
    def __init__(self, path):
        self.lock = threading.Lock()
        self.file = open(path, 'w', encoding='utf-8')
        self.sequence = 0

    def write(self, entry):
        with self.lock:
            self.sequence += 1
            entry = {'seq': self.sequence, 'at': time.time(), **entry}
            self.file.write(json.dumps(entry) + '\n')
            self.file.flush()

    def close(self):
        self.file.close()


def proxy_handler(upstream_port, recorder):
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def log_message(self, *args):
            pass

        def handle_any(self):
            length = int(self.headers.get('Content-Length') or 0)
            body = self.rfile.read(length) if length else b''
            try:
                message = json.loads(body) if body else None
            except ValueError:
                message = None
            rpc = message.get('method') if isinstance(message, dict) else None
            tool = None
            if rpc == 'tools/call':
                tool = (message.get('params') or {}).get('name')
            request = {'direction': 'request', 'http': self.command, 'path': self.path,
                       'rpc': rpc, 'tool': tool,
                       'id': message.get('id') if isinstance(message, dict) else None,
                       'authorized': self.headers.get('Authorization') == f'Bearer {TOKEN}'}
            recorder.write(request)
            if (self.command == 'POST' and tool and tool.startswith('mo-')
                    and request['authorized']):
                content = {'value': f'operator tool {tool}'}
                reply = {'jsonrpc': '2.0', 'id': message['id'], 'result': {
                    'content': [{'type': 'text', 'text': json.dumps(content)}],
                    'structuredContent': content, 'isError': False}}
                self.respond(200, {'Content-Type': 'application/json'},
                             json.dumps(reply).encode(), 'proxy', tool)
                return
            headers = {k: v for k, v in self.headers.items()
                       if k.lower() not in ('host', 'content-length', 'connection')}
            connection = http.client.HTTPConnection('127.0.0.1', upstream_port, timeout=60)
            connection.request(self.command, self.path, body, headers)
            response = connection.getresponse()
            data = response.read()
            out_headers = {k: v for k, v in response.getheaders()
                           if k.lower() not in ('content-length', 'connection',
                                                'transfer-encoding')}
            connection.close()
            if rpc == 'tools/list' and response.status == 200:
                reply = json.loads(data)
                if 'result' in reply:
                    reply['result']['tools'] = reply['result']['tools'] + OPERATOR_TOOLS
                    data = json.dumps(reply).encode()
            self.respond(response.status, out_headers, data, 'server', tool)

        def respond(self, status, headers, data, answered_by, tool):
            listed = None
            is_error = None
            try:
                reply = json.loads(data) if data else None
                result = reply.get('result') if isinstance(reply, dict) else None
                if isinstance(result, dict) and 'tools' in result:
                    listed = [t['name'] for t in result['tools']]
                if isinstance(result, dict) and 'isError' in result:
                    is_error = result['isError']
            except ValueError:
                pass
            recorder.write({'direction': 'response', 'status': status, 'answered_by': answered_by,
                            'tool': tool, 'listed': listed, 'is_error': is_error,
                            'bytes': len(data)})
            self.send_response(status)
            for k, v in headers.items():
                self.send_header(k, v)
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        do_POST = do_GET = do_DELETE = handle_any

    return Handler


def start_server(binary, data):
    process = subprocess.Popen(
        [str(binary), 'serve', str(HERE / 'clients' / 'main.mo'), '--port', '0', '--data',
         str(data)], env={**os.environ, 'PROBE_TOKEN': TOKEN}, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, bufsize=0)
    line = b''
    limit = time.monotonic() + 60
    while not line.endswith(b'\n'):
        if not select.select([process.stdout], [], [], limit - time.monotonic())[0]:
            raise RuntimeError('mo serve did not start')
        chunk = process.stdout.read(1)
        if not chunk:
            raise RuntimeError(process.stderr.read().decode())
        line += chunk
    return process, int(line.split(b':')[-1].split(b'/')[0])


def version(command):
    return subprocess.run(command, capture_output=True, text=True, timeout=60).stdout.strip()


def run_codex(url, workdir, out, approve=False):
    command = ['codex', 'exec', '--json', '--ephemeral', '--skip-git-repo-check',
               '--sandbox', 'read-only',
               '-c', f'mcp_servers.probe.url="{url}"',
               '-c', 'mcp_servers.probe.bearer_token_env_var="PROBE_TOKEN"',
               '-c', 'approval_policy="never"']
    if approve:
        command += ['-c', 'mcp_servers.probe.default_tools_approval_mode="approve"']
    command.append(PROMPT)
    return command, version(['codex', '--version']), run(command, workdir, out)


def run_codex_approve(url, workdir, out):
    return run_codex(url, workdir, out, approve=True)


def run_claude(url, workdir, out):
    config = workdir / 'mcp.json'
    config.write_text(json.dumps({'mcpServers': {'probe': {
        'type': 'http', 'url': url, 'headers': {'Authorization': 'Bearer ${PROBE_TOKEN}'}}}}))
    command = ['claude', '-p', PROMPT, '--mcp-config', str(config), '--strict-mcp-config',
               '--output-format', 'stream-json', '--verbose', '--no-session-persistence',
               '--allowedTools', 'mcp__probe__*']
    return command, version(['claude', '--version']), run(command, workdir, out)


def run(command, workdir, out):
    with open(out, 'w', encoding='utf-8') as stream:
        result = subprocess.run(command, cwd=workdir, stdout=stream, stderr=subprocess.PIPE,
                                text=True, timeout=900, stdin=subprocess.DEVNULL,
                                env={**os.environ, 'PROBE_TOKEN': TOKEN})
    return {'exit_status': result.returncode, 'stderr_tail': result.stderr[-1500:]}


def visible_names(client, path):
    """The probe tool names a client's init event lists (Claude Code only) and its calls."""
    names, calls = set(), []
    for line in pathlib.Path(path).read_text(encoding='utf-8').splitlines():
        try:
            event = json.loads(line)
        except ValueError:
            continue
        if client == 'claude':
            if event.get('type') == 'system' and event.get('subtype') == 'init':
                names.update(t for t in event.get('tools', []) if 'probe' in t)
            for block in (event.get('message') or {}).get('content', []) or []:
                if isinstance(block, dict) and block.get('type') == 'tool_use':
                    calls.append(block.get('name'))
        else:
            item = event.get('item') or {}
            if item.get('type') == 'mcp_tool_call' and event.get('type') == 'item.completed':
                calls.append(f"{item.get('server')}/{item.get('tool')}")
    return sorted(names), calls


def main(args):
    if len(args) < 3:
        raise SystemExit(__doc__)
    binary, out = pathlib.Path(args[0]).resolve(), pathlib.Path(args[1]).resolve()
    out.mkdir(parents=True, exist_ok=True)
    recorder = Recorder(out / 'traffic.jsonl')
    summary = {}
    with tempfile.TemporaryDirectory() as temporary:
        data = pathlib.Path(temporary) / 'data'
        data.mkdir()
        process, upstream = start_server(binary, data)
        proxy = http.server.ThreadingHTTPServer(('127.0.0.1', 0),
                                                proxy_handler(upstream, recorder))
        thread = threading.Thread(target=proxy.serve_forever, daemon=True)
        thread.start()
        url = f'http://127.0.0.1:{proxy.server_address[1]}/mcp'
        try:
            for client in args[2:]:
                workdir = pathlib.Path(temporary) / client
                workdir.mkdir()
                recorder.write({'direction': 'marker', 'client': client})
                runner = {'codex': run_codex, 'codex-approve': run_codex_approve,
                          'claude': run_claude}[client]
                command, client_version, result = runner(url, workdir, out / f'{client}.jsonl')
                names, calls = visible_names(client, out / f'{client}.jsonl')
                if client.startswith('codex'):
                    shown = command[:-1]
                else:
                    shown = command[:2] + ['PROMPT'] + command[3:]
                summary[client] = {'version': client_version, 'command': shown, **result,
                                   'visible_names': names, 'calls': calls}
        finally:
            proxy.shutdown()
            process.send_signal(signal.SIGTERM)
            process.wait(timeout=60)
            recorder.close()
    traffic = [json.loads(line) for line in (out / 'traffic.jsonl').read_text().splitlines()]
    summary['wire_listed'] = next((t['listed'] for t in traffic if t.get('listed')), None)
    summary['wire_calls'] = [(t['seq'], t['tool'], t.get('authorized')) for t in traffic
                             if t.get('direction') == 'request' and t.get('tool')]
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
