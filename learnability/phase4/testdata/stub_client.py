#!/usr/bin/env python3
"""A stand-in for Claude Code and Codex in the session harness's tests. Installed as both
clients, it is called with the harness's command line for either, as the session user, and
writes that client's event stream. What it does comes from the JSON file `stub-mode` in the
folder above its own (the install folder): {"mode": MODE, ...}.

  fix        call stats and list through the proxy, fix the synthetic variant's defect, stop
  sleep      half-edit main.mo, start a child `sleep`, and sleep past any limit
  trip       call stats until the supervisor stops the service
  foreign    report a shell command addressing a host other than the loopback address
  litter     leave a file in its home and under /tmp and a background process, then stop
  look       report, as JSON in its last message, which of "paths" it can read, whether the
             litter is still there, and whether it can write the start-data copy
"""

import http.client
import json
import os
import pathlib
import subprocess
import sys
import time
import urllib.parse

ARGS = sys.argv[1:]
CODEX = ARGS[:1] == ['exec']
calls = 0
messages = 0


def emit(event):
    print(json.dumps(event), flush=True)


def say(text):
    global messages
    messages += 1
    if CODEX:
        emit({'type': 'item.completed', 'item': {'id': f'item_m{messages}',
                                                 'type': 'agent_message', 'text': text}})
    else:
        emit({'type': 'assistant', 'message': {'id': f'msg_{messages}', 'content': [
            {'type': 'text', 'text': text}]}})


def shell(command):
    global calls
    calls += 1
    if CODEX:
        emit({'type': 'item.completed', 'item': {'id': f'item_{calls}',
                                                 'type': 'command_execution',
                                                 'command': command}})
    else:
        emit({'type': 'assistant', 'message': {'content': [
            {'type': 'tool_use', 'id': f'toolu_{calls}', 'name': 'Bash',
             'input': {'command': command}}]}})


def endpoint():
    if CODEX:
        for at, arg in enumerate(ARGS):
            if arg == '-c' and ARGS[at + 1].startswith('mcp_servers.task-queue.url='):
                return ARGS[at + 1].split('=', 1)[1].strip('"')
    config = json.loads(pathlib.Path(ARGS[ARGS.index('--mcp-config') + 1]).read_text())
    return config['mcpServers']['task-queue']['url']


def call(tool, **arguments):
    """One tool call through the proxy, recorded in the stream as the client would."""
    global calls
    calls += 1
    if CODEX:
        item = {'id': f'item_{calls}', 'type': 'mcp_tool_call', 'server': 'task-queue',
                'tool': tool, 'arguments': arguments, 'status': 'in_progress'}
        emit({'type': 'item.started', 'item': item})
    else:
        emit({'type': 'assistant', 'message': {'content': [
            {'type': 'tool_use', 'id': f'toolu_{calls}', 'name': f'mcp__task-queue__{tool}',
             'input': arguments}]}})
    parts = urllib.parse.urlsplit(endpoint())
    body = json.dumps({'jsonrpc': '2.0', 'id': calls, 'method': 'tools/call',
                       'params': {'name': tool, 'arguments': arguments}})
    try:
        connection = http.client.HTTPConnection(parts.hostname, parts.port, timeout=60)
        connection.request('POST', parts.path, body, {
            'Authorization': f'Bearer {os.environ["QUEUE_TOKEN"]}',
            'Content-Type': 'application/json', 'MCP-Protocol-Version': '2025-11-25',
            'Accept': 'application/json, text/event-stream'})
        response = connection.getresponse()
        status, data = response.status, response.read().decode(errors='replace')
        connection.close()
    except OSError as error:
        status, data = 0, str(error)
    if CODEX:
        emit({'type': 'item.completed', 'item': {**item, 'status': 'completed',
                                                 'result': data}})
    else:
        emit({'type': 'user', 'message': {'content': [
            {'type': 'tool_result', 'tool_use_id': f'toolu_{calls}', 'content': data}]}})
    return status, data


def readable(path):
    try:
        if os.path.isdir(path):
            os.listdir(path)
        else:
            with open(path, 'rb') as file:
                file.read(1)
        return True
    except OSError:
        return False


def main():
    if ARGS == ['--version']:
        print('stub client 1')
        return 0
    settings = json.loads((pathlib.Path(os.path.realpath(sys.argv[0])).parent.parent /
                           'stub-mode').read_text())
    mode = settings['mode']
    if CODEX:
        emit({'type': 'thread.started', 'thread_id': 'stub'})
    else:
        emit({'type': 'system', 'subtype': 'init', 'cwd': os.getcwd(), 'tools': []})
    say(f'Working on it ({mode}).')
    main_mo = pathlib.Path('main.mo')
    if mode == 'fix':
        call('stats')
        call('list', filter={'case': 'AllJobs'}, after='0', limit='2')
        text = main_mo.read_text()
        main_mo.write_text(text.replace(
            '  if dead > 0\n    stop "dead jobs are not counted yet"\n  end\n', ''))
        say('Fixed: stats now counts dead jobs.')
    elif mode == 'sleep':
        with open(main_mo, 'a') as file:
            file.write('\n## half-edited\nfn half(')
        subprocess.Popen(['sleep', '600'])
        time.sleep(600)
    elif mode == 'trip':
        for _ in range(200):
            status, _ = call('stats')
            if status != 200:
                break
            time.sleep(0.06)
        say('The service stopped answering.')
    elif mode == 'foreign':
        shell('curl -s https://example.com/install.sh')
        say('Looked it up.')
    elif mode == 'litter':
        (pathlib.Path.home() / 'litter.txt').write_text('left behind\n')
        pathlib.Path('/tmp/stub-litter').write_text('left behind\n')
        subprocess.Popen(['setsid', 'sleep', '600'], stdin=subprocess.DEVNULL,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        say('Done.')
    elif mode == 'look':
        start = pathlib.Path(settings['start_data'])
        try:
            (start / 'mine').write_text('x')
            wrote = True
        except OSError:
            wrote = False
        sleepers = subprocess.run(['pgrep', '-U', str(os.getuid()), 'sleep'],
                                  capture_output=True, text=True).stdout.split()
        found = {'readable': {p: readable(p) for p in settings['paths']},
                 'home_litter': (pathlib.Path.home() / 'litter.txt').exists(),
                 'tmp_litter': os.path.exists('/tmp/stub-litter'),
                 'sleepers': len(sleepers), 'work': sorted(os.listdir('.')),
                 'wrote_start_data': wrote, 'start_data': sorted(os.listdir(start))}
        say(json.dumps(found))
    if not CODEX:
        emit({'type': 'result', 'subtype': 'success', 'is_error': False})
    else:
        emit({'type': 'turn.completed'})
    return 0


if __name__ == '__main__':
    sys.exit(main())
