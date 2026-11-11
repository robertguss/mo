#!/usr/bin/env python3
"""Pin upgrade-tool names in frozen native clients against the real catalog.

Usage: client_probe.py MO OUT codex codex-approve claude
Set CODEX_FROZEN and CLAUDE_FROZEN to the frozen executable paths. Raw client
streams stay in OUT outside the repository; publish only the summary and traffic.
No upgrade credential is supplied, so calls exercise refusal routing only.
"""
import http.client
import http.server
import json
import os
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'learnability/phase4/preflight'))
import client_probe as probe

NAMES = ['mo-upgrade-open', 'mo-upgrade-page', 'mo-upgrade-commit']
EXPECTED = {
    'codex': ['mcp__probe__' + name.replace('-', '_') for name in NAMES],
    'claude': ['mcp__probe__' + name for name in NAMES],
}
probe.PROMPT = (
    'An MCP server named probe is connected. Use only its tools. Discover the '
    'three tools whose wire names are mo-upgrade-open, mo-upgrade-page and '
    'mo-upgrade-commit. Call each once, in that order, with an empty argument '
    'object. These are deliberate refusal probes: no upgrade credential is '
    'configured, and no upload or activation can occur. Do not retry refusals. '
    'Finally list each of those three tool names exactly as it appears to you '
    '(the complete mcp__probe__ name), and report each refusal or approval block.'
)


def forwarding(upstream, recorder):
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            body = self.rfile.read(int(self.headers.get('Content-Length', '0')))
            message = json.loads(body)
            tool = message.get('params', {}).get('name') if message.get('method') == 'tools/call' else None
            recorder.write({'direction': 'request', 'rpc': message.get('method'), 'tool': tool,
                            'authorized': self.headers.get('Authorization') == f'Bearer {probe.TOKEN}'})
            headers = {k: v for k, v in self.headers.items()
                       if k.lower() not in ('host', 'connection', 'content-length')}
            connection = http.client.HTTPConnection('127.0.0.1', upstream, timeout=60)
            try:
                connection.request('POST', '/mcp', body, headers)
                response = connection.getresponse()
                data = response.read()
                result = json.loads(data).get('result', {}) if data else {}
                catalog = result.get('tools')
                recorder.write({'direction': 'response', 'status': response.status,
                                'answered_by': 'server', 'tool': tool,
                                'listed': [t['name'] for t in catalog] if catalog else None,
                                'upgrade_catalog': [t for t in catalog if t['name'] in NAMES] if catalog else None,
                                'is_error': result.get('isError'),
                                'rpc_error': json.loads(data).get('error') if data else None,
                                'structured': result.get('structuredContent')})
                self.send_response(response.status)
                for key, value in response.getheaders():
                    if key.lower() not in ('connection', 'content-length', 'transfer-encoding'):
                        self.send_header(key, value)
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)
            finally:
                connection.close()
    return Handler


original_run = probe.run
original_version = probe.version


def executable(client):
    return os.environ['CODEX_FROZEN' if client == 'codex' else 'CLAUDE_FROZEN']


def frozen_version(command):
    return original_version([executable(command[0]), *command[1:]])


def frozen_run(command, workdir, out):
    if command[0] == 'codex':
        command[2:2] = ['--model', 'gpt-6.1-sol', '-c', 'model_reasoning_effort="xhigh"']
    else:
        command[3:3] = ['--model', 'claude-opus-5-5']
    command[0] = executable(command[0])
    return original_run(command, workdir, out)


probe.proxy_handler = forwarding
probe.run = frozen_run
probe.version = frozen_version


def main(args):
    for client, version in [('codex', 'codex-cli 0.166.0'), ('claude', '2.1.317 (Claude Code)')]:
        actual = frozen_version([client, '--version'])
        if actual != version:
            raise SystemExit(f'expected {version}, got {actual}')
    status = probe.main(args)
    out = pathlib.Path(args[1])
    summary = json.loads((out / 'summary.json').read_text())
    traffic = [json.loads(line) for line in (out / 'traffic.jsonl').read_text().splitlines()]
    for client in args[2:]:
        expected = EXPECTED['codex' if client.startswith('codex') else 'claude']
        assert set(expected) <= set(summary[client]['visible_names']), summary[client]
        assert summary[client]['exit_status'] == 0, summary[client]
    for client in args[2:]:
        events = [json.loads(line) for line in (out / f'{client}.jsonl').read_text().splitlines()
                  if line.startswith('{')]
        summary[client]['model_fallbacks'] = [
            {key: event.get(key) for key in ('trigger', 'original_model', 'fallback_model',
                                             'api_refusal_category')}
            for event in events if event.get('subtype') == 'model_refusal_fallback']
    catalog = next(t['upgrade_catalog'] for t in traffic if t.get('upgrade_catalog'))
    assert [t['name'] for t in catalog] == NAMES
    assert all(t['annotations']['readOnlyHint'] is False for t in catalog)
    assert catalog[-1]['annotations']['destructiveHint'] is True
    summary['pinned_upgrade_names'] = {'raw': NAMES, **EXPECTED}
    summary['upgrade_catalog'] = catalog
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    return status


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
