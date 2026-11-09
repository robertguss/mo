#!/usr/bin/python3
"""Unauthenticated DEVELOPMENT client; never install for real sessions."""
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import time

if '--version' in sys.argv:
    print('phase5 DEVELOPMENT stub 1')
    sys.exit(0)
root = Path(__file__).resolve().parents[1]
settings = json.loads((root / 'stub-settings.json').read_text())
def emit(value):
    print(json.dumps(value), flush=True)
emit({'type': 'session.started', 'model': 'gpt-6.1-sol'})
assert os.environ['QUEUE_TOKEN'] and os.environ['QUEUE_UPGRADE']
assert os.environ['QUEUE_UPGRADE_BEARER'] == 'Bearer ' + os.environ['QUEUE_UPGRADE']
assert json.loads((root / 'session/mcp.json').read_text())['mcpServers']['task-queue']['headers']['Mo-Upgrade-Authorization'] == 'Bearer ${QUEUE_UPGRADE}'
assert not os.access(settings['hidden'], os.R_OK)
assert os.access('../service.log', os.R_OK)
assert not os.access('../service.log', os.W_OK)
# The service command exposes its port via the active owner firewall rule, supplied
# by the operator-side test hook; the connection itself runs as the session UID.
import socket
try:
    socket.create_connection(('127.0.0.1', settings['port']), timeout=2)
except OSError:
    pass
else:
    raise AssertionError('backend reachable')
Path('isolation.json').write_text(json.dumps({'hidden_unreadable': True, 'backend_denied': True,
                                           'both_credentials': True}))
if settings['sleep']:
    Path('partial.txt').write_text('captured before timeout')
    subprocess.Popen(['sleep', '600'])
    time.sleep(600)
config = json.loads((root / 'session/mcp.json').read_text())
url = config['mcpServers']['task-queue']['url']
exit_code = 0
if not settings.get('noop', False):
    for source in (root / 'reference').glob('*.mo'):
        shutil.copyfile(source, Path.cwd() / source.name)
    checked = subprocess.run(['mo', 'check', 'main.mo', '--upgrade-from',
        str(root / 'session/deployed/main.mo'), '--migrate', 'migrate', '--json'], text=True, capture_output=True)
    if checked.returncode:
        print(checked.stdout + checked.stderr, file=sys.stderr)
        sys.exit(1)
    identity = json.loads(checked.stdout)
    config = json.loads((root / 'session/mcp.json').read_text())
    url = config['mcpServers']['task-queue']['url']
    cmd = ['mo', 'push', 'main.mo', '--url', url, '--base', identity['base_source'],
           '--migrate', 'migrate', '--token-env', 'QUEUE_TOKEN', '--upgrade-token-env', 'QUEUE_UPGRADE', '--json']
    result = subprocess.run(cmd, capture_output=True, text=True)
    emit({'type': 'item.completed', 'item': {'id': 'stub-push', 'status': 'completed', 'type': 'command_execution', 'command': shlex.join(cmd),
         'exit_code': result.returncode, 'aggregated_output': result.stdout + result.stderr}})
    exit_code = result.returncode
# Previously guessed literal 1; now print only the received ordinary HTTP result.
import urllib.request
payload = json.dumps({'jsonrpc': '2.0', 'id': 'stub-revision', 'method': 'tools/call', 'params': {'name': 'mo-state' if settings.get('noop', False) else 'revision', 'arguments': {}}})
headers = {'Content-Type': 'application/json', 'Accept': 'application/json, text/event-stream', 'MCP-Protocol-Version': '2025-11-25', 'Authorization': 'Bearer ' + os.environ['QUEUE_TOKEN']}
query = settings.get('query', 'native')
if query == 'native':
    request = urllib.request.Request(url, data=payload.encode(), headers=headers)
    with urllib.request.urlopen(request) as response:
        body = json.load(response)
else:
    if query == 'curl':
        command = ['/usr/bin/curl', '--silent', '--show-error', '--fail', url, '--data-binary', payload]
        for name, value in headers.items(): command.extend(['-H', name + ': ' + value])
    elif query == 'urllib':
        command = [sys.executable, '-c',
            'import json,os,sys,urllib.request; '
            'h=json.loads(sys.argv[3]); h["Authorization"]="Bearer "+os.environ["QUEUE_TOKEN"]; '
            'r=urllib.request.Request(sys.argv[1],data=sys.argv[2].encode(),headers=h); '
            'sys.stdout.write(urllib.request.urlopen(r).read().decode())',
            url, payload, json.dumps({k:v for k,v in headers.items() if k != 'Authorization'})]
    else:
        raise ValueError('unknown development query path')
    queried = subprocess.run(command, capture_output=True, text=True, check=True)
    assert not queried.stderr
    body = json.loads(queried.stdout)
assert not body.get('error') and not body['result'].get('isError')
observed = body['result']['structuredContent'].get('value', 'No upgrade attempted')
# Development-only native envelope; this is not a model qualification run.
if query == 'native':
    emit({'type': 'item.completed', 'item': {
        'id': 'stub-native-revision', 'type': 'mcp_tool_call', 'server': 'task-queue',
        'tool': 'mo-state' if settings.get('noop', False) else 'revision', 'arguments': {}, 'status': 'completed', 'error': None,
        'result': {'content': body['result'].get('content', []),
                   'structured_content': body['result']['structuredContent']}}})
else:
    emit({'type': 'item.completed', 'item': {
        'id': 'stub-shell-revision', 'type': 'command_execution', 'status': 'completed',
        'exit_code': 0, 'aggregated_output': queried.stdout}})
if settings.get('compile-failure', False):
    assert exit_code == 0 and str(observed) == '1'
    with Path('job-queue.mo').open('a') as source:
        source.write('\n@@@ invalid development syntax\n')
emit({'type': 'item.completed', 'item': {'type': 'agent_message', 'text': str(observed)}})
emit({'type': 'turn.completed'})
sys.exit(exit_code)
