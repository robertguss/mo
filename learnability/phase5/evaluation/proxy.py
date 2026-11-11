"""Phase5 recording wrapper; Phase4 transport and scanner remain unchanged."""
import hashlib
import importlib.util
import pathlib
import http.server
import threading
import shlex
import json
import os

PHASE4 = pathlib.Path(__file__).resolve().parents[2] / 'phase4'


def load(name):
    spec = importlib.util.spec_from_file_location('phase4_' + name, PHASE4 / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


base = load('proxy')
scan = load('scan')


def digests(data):
    result = {'body_byte_count': len(data),
              'body_sha256': hashlib.sha256(data).hexdigest()}
    try:
        value = json.loads(data)
        result['canonical_json_sha256'] = hashlib.sha256(json.dumps(
            value, sort_keys=True, separators=(',', ':'), ensure_ascii=True
        ).encode('ascii')).hexdigest()
    except (ValueError, UnicodeError):
        pass
    return result


class Recorder(base.Recorder):
    def __init__(self, path, tokens):
        if len(tokens) < 2 or len(set(tokens)) != len(tokens) or any(len(t) < 16 for t in tokens):
            raise ValueError('two distinct credentials of at least 16 bytes are required')
        super().__init__(path, '')
        os.chmod(path, 0o600)
        self.snapshots = []
        self.context = threading.local()
        self.patterns = scan.patterns(list(enumerate((t.encode() for t in tokens), 1)))

    def clean(self, value):
        if isinstance(value, str):
            raw = value.encode()
            decoded, _ = scan.decode(raw, True)
            if any(p in raw or p in decoded for p in self.patterns):
                return '[redacted]'
        elif isinstance(value, dict):
            return {self.clean(k): self.clean(v) for k, v in value.items()}
        elif isinstance(value, list):
            return [self.clean(v) for v in value]
        return value

    def write(self, entry):
        if entry['direction'] == 'request':
            self.snapshots.append(pathlib.Path('/proc/net/tcp').read_text())
        metadata = getattr(self.context, 'metadata', None)
        if metadata is not None:
            response = dict(getattr(self.context, 'response_metadata', {}))
            if entry['direction'] == 'response' and not response:
                text = entry.get('body')
                raw = b'' if text is None else text.encode() if isinstance(text, str) else None
                if raw is not None and len(raw) == entry.get('bytes'):
                    response = digests(raw)
            entry = {**entry, 'transport': dict(metadata) if entry['direction'] == 'request' else response}
        cleaned = self.clean(entry)
        if cleaned != entry:
            cleaned['redacted'] = True
        return super().write(cleaned)


def handler(upstream, recorder, token):
    class Connection(base.http.client.HTTPConnection):
        def getresponse(self):
            response = super().getresponse()
            read = response.read
            def captured_read(*args, **kwargs):
                data = read(*args, **kwargs)
                recorder.context.response_metadata = digests(data)
                return data
            response.read = captured_read
            return response

    class Handler(base.handler(upstream, recorder, token)):
        def setup(self):
            super().setup()
            self.request_ordinal = 0

        def body(self):
            data = super().body()
            self.request_ordinal += 1
            recorder.context.response_metadata = {}
            if self.upstream is None:
                parts = base.urllib.parse.urlsplit(upstream)
                self.upstream = Connection(parts.hostname, parts.port, timeout=300)
            remote = self.connection.getpeername()
            local = self.connection.getsockname()
            recorder.context.metadata = {
                'endpoint': f'{remote[0]}:{remote[1]}->{local[0]}:{local[1]}',
                'request_ordinal': self.request_ordinal, **digests(data)}
            return data

        def handle_any(self):
            # Capture response bytes at the existing parser boundary, without
            # changing forwarding, headers, body framing or credential handling.
            super().handle_any()

        do_POST = do_GET = do_DELETE = do_PUT = handle_any
    return Handler


class Proxy(base.Proxy):
    def __init__(self, upstream, log, service_token, upgrade_token, client_secrets=()):
        self.recorder = Recorder(log, [service_token, upgrade_token, *client_secrets])
        self.server = http.server.ThreadingHTTPServer(
            ('127.0.0.1', 0), handler(upstream, self.recorder, service_token))
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = f'http://127.0.0.1:{self.server.server_address[1]}/mcp'


UPLOAD = {'mo-upgrade-open', 'mo-upgrade-page', 'mo-upgrade-commit'}
def is_push(command, depth=0):
    if depth > 4:
        return False
    try:
        if isinstance(command, list):
            words = command
        else:
            lexer = shlex.shlex(command, posix=True, punctuation_chars=';&|()')
            lexer.whitespace_split = True
            words = list(lexer)
    except (TypeError, ValueError):
        return False
    start = True
    for i, word in enumerate(words):
        if word in (';', '&&', '||', '|', '&', '(', ')'):
            start = True
            continue
        if not start:
            continue
        if '=' in word and word.split('=', 1)[0].isidentifier():
            continue
        name = pathlib.PurePosixPath(word).name
        if name in ('env', 'command', 'exec'):
            continue
        start = False
        if name == 'mo' and words[i + 1:i + 2] == ['push']:
            return True
        if name in ('sh', 'bash', 'zsh') and i + 2 < len(words):
            if words[i + 1] in ('-c', '-lc') and is_push(words[i + 2], depth + 1):
                return True
    return False


def match(client, stream, log, *, capture=None, session_id=None, evaluation_endpoint=None):
    """Reconcile completed native occurrences; shell traffic needs independent evidence.

    Command text, request headers and arbitrary origin fields are never evidence.
    Until a qualified collector supplies sender evidence, shell requests fail closed.
    """
    rows = base.entries(log)
    origins = {}
    unanswered = []
    observation = False
    if capture is not None:
        import origin_identity
        import origin_join
        from protected_regrade import ReplayReceipt
        if type(capture) not in (origin_identity.Capture, ReplayReceipt):
            raise ValueError('collector Capture required')
        evidence = capture.consume(session_id, evaluation_endpoint=evaluation_endpoint)
        if evidence['integrity']['process_completeness_verified'] is not True:
            raise ValueError('incomplete process capture')
        if evidence['proxy_rows'] != rows:
            raise ValueError('sealed proxy log differs')
        joined = origin_join.join(evidence['trace'], rows)
        unanswered = joined['unanswered']
        requests = {(r['connection_ordinal'], r['request_ordinal']): r
                    for r in evidence['trace']['requests']}
        for observation in joined['observations']:
            receipt = requests[observation['connection_ordinal'], observation['request_ordinal']]
            origins[observation['request_seq']] = receipt

    provenance = dict(qualified=False, reason='no collector capture')
    if capture is not None:
        import observation as u3
        from model_adapter import session_provenance
        observation = u3.validate(evidence, rows, origins, stream.read_bytes())
        try:
            provenance = session_provenance(client, evidence, stream.read_bytes())
        except Exception as error:
            provenance = dict(qualified=False, reason='provenance evaluation error: ' + type(error).__name__)
    responses = {}
    for row in rows:
        if row.get('direction') == 'response':
            responses.setdefault(row.get('request'), []).append(row)
    remaining = list(base.stream_calls(client, stream, base.SERVER))
    result = dict(revision_observed=observation, model_provenance=provenance, observation_coverage="typed native results; exact single-writer pipe delivery to typed shell results; or shell output containing the exact response body that the receiving process wrote to its standard output and the client then read from the same pipe or file before emitting the typed result; other framing rejects", pairs=0, log_only=[], stream_only=[], exempt=[], shell=0, unanswered_requests=unanswered,
                  command_origin=[], shell_origins=[], upload_routes=dict(native=0, shell=0),
                  verified_push_requests=sum(r['verified_push_identity'] for seq, r in origins.items()
                                             if seq not in unanswered),
                  verified_shell_requests=len(origins) - len(unanswered))
    for row in rows:
        if row.get('direction') != 'request' or row.get('rpc') != 'tools/call':
            continue
        call = {'log_id': row.get('id'), 'request_seq': row.get('seq'),
                'tool': row.get('tool')}
        replies = responses.get(row.get('seq'), [])
        if len(replies) != 1:
            result['exempt'].append(call)
            continue
        arguments = ((row.get('body') or {}).get('params') or {}).get('arguments')
        twin = next((r for r in remaining if r[3] and r[1] == row.get('tool')
                     and base.canonical(r[2]) == base.canonical(arguments)), None)
        if row.get('tool') in UPLOAD:
            result['upload_routes']['shell' if twin is None else 'native'] += 1
        if twin is None:
            receipt = origins.get(row.get('seq'))
            if receipt is None:
                result['log_only'].append(call)
            else:
                result['shell'] += 1
                call['origin'] = 'shell'
                result['shell_origins'].append(call)
                if receipt['verified_push_identity']:
                    result['command_origin'].append(call)

        else:
            remaining.remove(twin)
            result['pairs'] += 1
    for ident, tool, _, answered in remaining:
        result['stream_only' if answered else 'exempt'].append(
            {'stream_id': ident, 'tool': tool})
    result['match'] = not any(result[k] for k in ('log_only', 'stream_only', 'exempt'))
    return result
