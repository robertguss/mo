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
emit({'type': 'item.completed', 'item': {'type': 'command_execution', 'command': shlex.join(cmd),
     'exit_code': result.returncode, 'aggregated_output': result.stdout + result.stderr}})
emit({'type': 'item.completed', 'item': {'type': 'agent_message', 'text': '1'}})
emit({'type': 'turn.completed'})
sys.exit(result.returncode)
