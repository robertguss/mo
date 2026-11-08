"""Phase5 recording wrapper; Phase4 transport and scanner remain unchanged."""
import importlib.util
import pathlib
import http.server
import threading
import shlex

PHASE4 = pathlib.Path(__file__).resolve().parents[2] / 'phase4'


def load(name):
    spec = importlib.util.spec_from_file_location('phase4_' + name, PHASE4 / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


base = load('proxy')
scan = load('scan')


class Recorder(base.Recorder):
    def __init__(self, path, tokens):
        if len(tokens) < 2 or len(set(tokens)) != len(tokens) or any(len(t) < 16 for t in tokens):
            raise ValueError('two distinct credentials of at least 16 bytes are required')
        super().__init__(path, '')
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
        cleaned = self.clean(entry)
        if cleaned != entry:
            cleaned['redacted'] = True
        return super().write(cleaned)


class Proxy(base.Proxy):
    def __init__(self, upstream, log, service_token, upgrade_token, client_secrets=()):
        self.recorder = Recorder(log, [service_token, upgrade_token, *client_secrets])
        self.server = http.server.ThreadingHTTPServer(
            ('127.0.0.1', 0), base.handler(upstream, self.recorder, service_token))
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


def match(client, stream, log):
    """Keep unmatched uploads explicit; a recorded shell push gives command attribution.

    As in Phase4 fetch attribution, this cannot distinguish curl beside a push.
    The command and individual requests remain in the private record for review.
    """
    result = base.match(client, stream, log)
    pushed = any(kind == 'command' and is_push(command)
                 for kind, command in base.commands(client, stream))
    result['command_origin'] = []
    if pushed:
        for call in list(result['log_only']):
            if call['tool'] in UPLOAD:
                result['log_only'].remove(call)
                result['command_origin'].append({**call, 'command': 'mo push'})
    result['match'] = not result['log_only'] and not result['stream_only'] and not result['exempt']
    return result
