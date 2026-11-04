#!/usr/bin/env python3
"""The recording proxy between a Phase 4 session's client and its service, and the checks that
read its log beside the client's own event stream.

  python3 learnability/phase4/proxy.py serve UPSTREAM LOG TOKEN_ENV
        listen on a free port of 127.0.0.1, print `listening on URL` once ready, forward every
        request to UPSTREAM (the service's http://127.0.0.1:PORT/mcp) and record it and its
        response in LOG, until SIGTERM or SIGINT. TOKEN_ENV names the environment variable
        holding the session's token, which the proxy needs only to record whether a request
        carried it.
  python3 learnability/phase4/proxy.py match CLIENT STREAM LOG [SERVER]
        whether the client's event stream and the proxy log hold the same tool calls. CLIENT is
        claude or codex; SERVER is the MCP server's name in the client's configuration
        (task-queue). Prints one JSON object; exit 0 when they match, 1 when not.
  python3 learnability/phase4/proxy.py hosts CLIENT STREAM
        every host other than the loopback address that the stream's shell commands and web
        tools address, with the command that named it. Prints JSON; exit 0 when there is none,
        1 when there is any.

The log has one JSON object per line, written under a lock in the order the proxy saw them.
A request: seq, at (seconds since the epoch), direction "request", connection (a number per
client connection), http (the method), path, rpc (the JSON-RPC method), id, tool (for
tools/call), body (the whole request, parsed when it is JSON) and authorized (whether it carried
the session's token). A response: seq, at, direction "response", request (its request's seq),
connection, rpc, id, tool, status, elapsed (seconds), bytes, body, is_error, error (the runtime
error's code, or the JSON-RPC error's) and listed (the tool names of a tools/list reply). A
request the service could not be reached for is answered 502 and recorded with that status.
The Authorization header is forwarded unchanged and never written; a token that appears
anywhere else in a line is written as [token] and the line gets "redacted": true.

The two records match when every tools/call in the log pairs with a tool call of that server in
the client's stream, and the reverse. Neither client's stream carries the JSON-RPC id, so a call
pairs with one of the same tool and the same arguments; an unpaired call is named by its
JSON-RPC id (log) or its stream id. A call still in flight when the session ended, a
request with no response or a stream call with no result, is exempt. initialize, tools/list
and notifications have no stream counterpart and are not paired.
"""

import http.client
import http.server
import itertools
import json
import os
import pathlib
import re
import shlex
import signal
import sys
import threading
import time
import urllib.parse

CLIENTS = ('claude', 'codex')
SERVER = 'task-queue'
HOP = ('connection', 'keep-alive', 'transfer-encoding', 'content-length', 'host',
       'proxy-connection', 'te', 'trailer', 'upgrade')


class Recorder:
    """Writes log entries, one JSON line each, numbered in the order they are written."""

    def __init__(self, path, token):
        self.lock = threading.Lock()
        self.file = open(path, 'w', encoding='utf-8')
        self.token = token
        self.sequence = 0

    def write(self, entry):
        with self.lock:
            if self.file.closed:
                return None
            self.sequence += 1
            entry = {'seq': self.sequence, 'at': time.time(), **entry}
            line = json.dumps(entry)
            if self.token and self.token in line:
                line = json.dumps({**json.loads(line.replace(self.token, '[token]')),
                                   'redacted': True})
            self.file.write(line + '\n')
            self.file.flush()
            return self.sequence

    def close(self):
        with self.lock:
            self.file.close()


def parsed(data):
    """A body as JSON when it is JSON, else as text."""
    if not data:
        return None
    try:
        return json.loads(data)
    except ValueError:
        return data.decode('utf-8', errors='replace')


def reply_fields(body):
    """is_error, error and listed from a response body."""
    is_error = error = listed = None
    if isinstance(body, dict):
        if isinstance(body.get('error'), dict):
            error = body['error'].get('code')
        result = body.get('result')
        if isinstance(result, dict):
            if isinstance(result.get('tools'), list):
                listed = [t.get('name') for t in result['tools'] if isinstance(t, dict)]
            if 'isError' in result:
                is_error = result['isError']
            content = result.get('structuredContent')
            if is_error and isinstance(content, dict) and isinstance(content.get('error'), dict):
                error = content['error'].get('code') or content['error'].get('kind')
    return is_error, error, listed


def handler(upstream, recorder, token):
    parts = urllib.parse.urlsplit(upstream)
    connections = itertools.count(1)

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def log_message(self, *args):
            pass

        def setup(self):
            super().setup()
            self.number = next(connections)
            self.upstream = None

        def finish(self):
            super().finish()
            if self.upstream is not None:
                self.upstream.close()

        def body(self):
            if 'chunked' in self.headers.get('Transfer-Encoding', '').lower():
                data = b''
                while True:
                    size = int(self.rfile.readline().split(b';')[0].strip() or b'0', 16)
                    if size == 0:
                        while self.rfile.readline() not in (b'\r\n', b'\n', b''):
                            pass  # trailers
                        return data
                    data += self.rfile.read(size)
                    self.rfile.readline()
            length = int(self.headers.get('Content-Length') or 0)
            return self.rfile.read(length) if length else b''

        def handle_any(self):
            data = self.body()
            message = parsed(data)
            rpc = message.get('method') if isinstance(message, dict) else None
            ident = message.get('id') if isinstance(message, dict) else None
            tool = None
            if rpc == 'tools/call' and isinstance(message.get('params'), dict):
                tool = message['params'].get('name')
            authorized = bool(token) and self.headers.get('Authorization') == f'Bearer {token}'
            request = recorder.write({
                'direction': 'request', 'connection': self.number, 'http': self.command,
                'path': self.path, 'rpc': rpc, 'id': ident, 'tool': tool, 'body': message,
                'authorized': authorized})
            started = time.monotonic()
            headers = {k: v for k, v in self.headers.items() if k.lower() not in HOP}
            try:
                if self.upstream is None:
                    self.upstream = http.client.HTTPConnection(parts.hostname, parts.port,
                                                               timeout=300)
                self.upstream.request(self.command, self.path, data, headers)
                response = self.upstream.getresponse()
                status, reply = response.status, response.read()
                out = [(k, v) for k, v in response.getheaders() if k.lower() not in HOP]
            except (OSError, http.client.HTTPException) as error:
                if self.upstream is not None:
                    self.upstream.close()
                    self.upstream = None
                status, out = 502, [('Content-Type', 'text/plain')]
                reply = f'the service could not be reached: {error}'.encode()
            body = parsed(reply)
            is_error, error, listed = reply_fields(body)
            recorder.write({
                'direction': 'response', 'request': request, 'connection': self.number,
                'rpc': rpc, 'id': ident, 'tool': tool, 'status': status,
                'elapsed': round(time.monotonic() - started, 6), 'bytes': len(reply),
                'body': body, 'is_error': is_error, 'error': error, 'listed': listed})
            self.send_response(status)
            for k, v in out:
                self.send_header(k, v)
            self.send_header('Content-Length', str(len(reply)))
            self.end_headers()
            self.wfile.write(reply)

        do_POST = do_GET = do_DELETE = do_PUT = handle_any

    return Handler


class Proxy:
    """A running proxy: its URL, and stop()."""

    def __init__(self, upstream, log, token):
        self.recorder = Recorder(log, token)
        self.server = http.server.ThreadingHTTPServer(
            ('127.0.0.1', 0), handler(upstream, self.recorder, token))
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = f'http://127.0.0.1:{self.server.server_address[1]}/mcp'

    def stop(self):
        self.server.shutdown()
        self.server.server_close()
        self.recorder.close()


def entries(log):
    return list(events(log))


def log_calls(log):
    """Each tools/call in the log as (id, tool, arguments, answered)."""
    found = entries(log)
    answered = {e.get('request') for e in found if e.get('direction') == 'response'}
    calls = []
    for e in found:
        if e.get('direction') == 'request' and e.get('rpc') == 'tools/call':
            params = (e.get('body') or {}).get('params') or {}
            calls.append((e.get('id'), e.get('tool'), params.get('arguments'),
                          e['seq'] in answered))
    return calls


def events(stream):
    for line in pathlib.Path(stream).read_text(encoding='utf-8', errors='replace').splitlines():
        try:
            event = json.loads(line)
        except ValueError:
            continue
        if isinstance(event, dict):
            yield event


def content_blocks(event):
    content = (event.get('message') or {}).get('content')
    return [b for b in content if isinstance(b, dict)] if isinstance(content, list) else []


def arguments_of(value):
    if isinstance(value, str):
        try:
            return json.loads(value)
        except ValueError:
            return value
    return value


def stream_calls(client, stream, server):
    """Each call the client made to SERVER's tools as (stream id, tool, arguments, answered)."""
    calls, done = {}, set()
    if client == 'claude':
        prefix = f'mcp__{server}__'
        for event in events(stream):
            for block in content_blocks(event):
                if block.get('type') == 'tool_use' and str(block.get('name')).startswith(prefix):
                    calls.setdefault(block.get('id'), (block['name'][len(prefix):],
                                                       block.get('input')))
                elif block.get('type') == 'tool_result':
                    done.add(block.get('tool_use_id'))
    elif client == 'codex':
        for event in events(stream):
            item = event.get('item') or {}
            if item.get('type') == 'mcp_tool_call' and item.get('server') == server:
                calls.setdefault(item.get('id'), (item.get('tool'),
                                                  arguments_of(item.get('arguments'))))
                if event.get('type') == 'item.completed':
                    done.add(item.get('id'))
    else:
        raise ValueError(f'unknown client {client!r}')
    return [(ident, tool, arguments, ident in done) for ident, (tool, arguments) in calls.items()]


def canonical(arguments):
    return json.dumps(arguments, sort_keys=True)


def match(client, stream, log, server=SERVER):
    """Pairs the log's tools/call requests with the stream's tool calls."""
    logged, streamed = log_calls(log), stream_calls(client, stream, server)
    log_only, stream_only, exempt, pairs = [], [], [], 0
    for tool in sorted({c[1] for c in logged} | {c[1] for c in streamed}, key=str):
        left = [c for c in logged if c[1] == tool]
        right = [c for c in streamed if c[1] == tool]
        for call in list(left):
            twin = next((r for r in right if canonical(r[2]) == canonical(call[2])), None)
            if twin is not None:
                left.remove(call)
                right.remove(twin)
                pairs += 1
        for ident, _, _, answered in left:
            (log_only if answered else exempt).append({'log_id': ident, 'tool': tool})
        for ident, _, _, answered in right:
            (stream_only if answered else exempt).append({'stream_id': ident, 'tool': tool})
    return {'match': not log_only and not stream_only, 'pairs': pairs, 'log_only': log_only,
            'stream_only': stream_only, 'exempt': exempt}


LOOPBACK = re.compile(r'^(127(\.\d{1,3}){3}|localhost|::1|\[::1\])$', re.IGNORECASE)
URL = re.compile(r'\b[a-zA-Z][a-zA-Z0-9+.-]*://(?:[^\s@/\'"]*@)?(\[[0-9a-fA-F:.]+\]|'
                 r'[^\s/:\'"?#\\]+)')
USER_AT = re.compile(r'(?<![\w.%+-])[\w.-]+@((?:[A-Za-z0-9-]+\.)+[A-Za-z]{2,}|'
                     r'\d{1,3}(?:\.\d{1,3}){3})\b')
IPV4 = re.compile(r'(?<![\w.])(\d{1,3}(?:\.\d{1,3}){3})(?![\w.])')
# Commands whose plain words name hosts, and the options of curl and wget that take a value.
NAMING = {'curl', 'wget', 'ssh', 'scp', 'sftp', 'rsync', 'nc', 'ncat', 'netcat', 'telnet',
          'ping', 'ftp', 'dig', 'nslookup', 'host', 'http', 'https', 'xh'}
VALUED = {'-o', '-O', '-d', '-H', '-X', '-u', '-T', '-F', '-w', '-K', '-b', '-c', '-e', '-A',
          '-m', '-P', '-p', '-i', '-l', '--output', '--data', '--data-raw', '--header',
          '--request', '--user', '--config', '--output-document', '--max-time', '--port'}
HOSTLIKE = re.compile(r'^(?:[A-Za-z0-9-]+\.)+[A-Za-z][A-Za-z0-9-]*$|^\d{1,3}(?:\.\d{1,3}){3}$')
# A word ending in one of these names a host wherever it stands; the list leaves out every
# ending a file name here can have (.mo, .md, .py, .sh, .json, .log, ...).
DOMAINS = {'com', 'org', 'net', 'io', 'dev', 'ai', 'gov', 'edu', 'info', 'xyz', 'cloud', 'uk',
           'eu', 'jp', 'cn', 'ru', 'fr'}
SEPARATOR = re.compile(r'(\|\||&&|\$\(|[;|&()`])')
SEPARATORS = {';', '|', '&', '&&', '||', '$(', '(', ')', '`'}


def words(command):
    try:
        return shlex.split(command, comments=False)
    except ValueError:
        return command.split()


def named_hosts(command):
    """The hosts a shell command addresses: in URLs, as user@host, as an IPv4 address, as a
    word with a domain ending, or as a plain word after a command that takes a host, looking
    inside quoted commands such as `bash -lc '...'` and `python3 -c "..."` too."""
    found = [m.group(1) for m in URL.finditer(command)]
    found += [m.group(1) for m in USER_AT.finditer(command)]
    found += [m.group(1) for m in IPV4.finditer(command)]
    found += word_hosts(command, 0)
    return [h.strip('[]').lower() if h.startswith('[') else h.lower() for h in found]


def word_hosts(text, depth):
    found, naming, skip = [], False, False
    for word in words(SEPARATOR.sub(r' \1 ', text)):
        if word in SEPARATORS:
            naming = False
            continue
        if skip:
            skip = False
            continue
        if depth < 3 and re.search(r'\s', word):
            found += word_hosts(word, depth + 1)
            continue
        if os.path.basename(word) in NAMING:
            naming = True
            continue
        if word.startswith('-') or '://' in word:
            skip = naming and word in VALUED
            continue
        host = word.split('@')[-1].split(':')[0].split('/')[0].strip(',\'"')
        if HOSTLIKE.match(host) and (naming or host.rsplit('.', 1)[-1].lower() in DOMAINS):
            found.append(host)
    return found


def commands(client, stream):
    """Each shell command and web tool use in the stream, as (what, text)."""
    for event in events(stream):
        if client == 'claude':
            for block in content_blocks(event):
                if block.get('type') != 'tool_use':
                    continue
                given = block.get('input') or {}
                if block.get('name') == 'Bash':
                    yield 'command', str(given.get('command', ''))
                elif block.get('name') == 'WebFetch':
                    yield 'web fetch', str(given.get('url', ''))
                elif block.get('name') == 'WebSearch':
                    yield 'web search', str(given.get('query', ''))
        elif event.get('type') in ('item.started', 'item.completed'):
            item = event.get('item') or {}
            if item.get('type') == 'command_execution':
                command = item.get('command')
                yield 'command', ' '.join(command) if isinstance(command, list) else str(command)
            elif item.get('type') == 'web_search':
                yield 'web search', str(item.get('query', ''))


def hosts(client, stream):
    """Every host other than the loopback address the stream addresses, with its command."""
    found, seen = [], set()
    for what, text in commands(client, stream):
        named = ['web search'] if what == 'web search' else named_hosts(text)
        for host in named:
            if not LOOPBACK.match(host) and (host, text) not in seen:
                seen.add((host, text))
                found.append({'host': host, what: text})
    return found


def serve(upstream, log, token_env):
    token = os.environ.get(token_env)
    if not token:
        print(f'proxy: the environment variable {token_env} holds no token', file=sys.stderr)
        return 2
    proxy = Proxy(upstream, log, token)
    stopped = threading.Event()
    for name in (signal.SIGTERM, signal.SIGINT):
        signal.signal(name, lambda *_: stopped.set())
    print(f'listening on {proxy.url}', flush=True)
    stopped.wait()
    proxy.stop()
    return 0


def main(args):
    if len(args) == 4 and args[0] == 'serve':
        return serve(args[1], args[2], args[3])
    if len(args) in (4, 5) and args[0] == 'match' and args[1] in CLIENTS:
        result = match(args[1], args[2], args[3], *args[4:])
        print(json.dumps(result, indent=2))
        return 0 if result['match'] else 1
    if len(args) == 3 and args[0] == 'hosts' and args[1] in CLIENTS:
        found = hosts(args[1], args[2])
        print(json.dumps(found, indent=2))
        return 1 if found else 0
    print(__doc__.split('\n\n')[1], file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
