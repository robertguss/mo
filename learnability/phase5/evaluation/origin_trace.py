"""Bounded collector strace decoder. Exceptions never include trace contents.

Supports PID-prefixed or single-PID traces, strace C strings, scalar TCP reads/writes,
unfinished/resumed calls and Content-Length HTTP/1.x. No binary authentication.
Only a completely parsed trace returns receipts. Callers must establish collector
ownership, trace completeness and executable hashes independently.
"""
import hashlib
import argparse
import collections
import json
import re
from trace_storage import TraceLines, StorageError


class TraceError(ValueError):
    pass


def fail(reason):
    raise TraceError(reason)


def decode(value):
    out = bytearray()
    escapes = dict(zip('abfnrtv\\"', [7,8,12,10,13,9,11,92,34]))
    i = 0
    while i < len(value):
        ch = value[i]; i += 1
        if ch != '\\':
            if not 32 <= ord(ch) <= 126: fail('unsupported string encoding')
            out.append(ord(ch)); continue
        if i == len(value): fail('truncated string escape')
        ch = value[i]; i += 1
        if ch in escapes: out.append(escapes[ch])
        elif ch == 'x':
            digits = value[i:i+2]
            if not re.fullmatch('[0-9a-fA-F]{2}', digits): fail('invalid hex escape')
            out.append(int(digits,16)); i += 2
        elif ch in '01234567':
            digits = ch
            while i < len(value) and len(digits)<3 and value[i] in '01234567':
                digits += value[i]; i += 1
            if int(digits,8)>255: fail('invalid octal escape')
            out.append(int(digits,8))
        else: fail('unsupported string escape')
    return bytes(out)


def transfer_bytes(op, args, n):
    """Decode exactly the successful prefix of scalar or explicit iovec buffers."""
    tail = re.sub(r'^\d+(?:<TCP:\[[^\]]+\]>|<[^>]+>)?,\s*', '', args, count=1)
    if op in ('read', 'write', 'sendto', 'recvfrom'):
        match = re.fullmatch(r'"((?:\\.|[^"\\])*)",\s*(\d+)(?:,.*)?', tail)
        if not match: fail('missing or truncated transfer buffer')
        data = decode(match[1])
        if n > len(data) or n > int(match[2]): fail('short captured transfer buffer')
        return data[:n]
    vector = r'\[((?:\{iov_base="(?:\\.|[^"\\])*", iov_len=\d+\}(?:, )?)*)\]'
    if op in ('readv', 'writev'):
        match = re.fullmatch(vector + r', (\d+)', tail)
    elif op in ('sendmsg', 'recvmsg'):
        # No ancillary descriptor delivery or alternate destination is supported.
        match = re.fullmatch(r'\{msg_name=NULL, msg_namelen=0, msg_iov=' + vector +
            r', msg_iovlen=(\d+), (?:msg_control=NULL, )?msg_controllen=0, msg_flags=0\}, [^,]+', tail)
    else:
        fail('unsupported transfer operation')
    if not match: fail('unsupported or truncated vector buffer')
    entries = re.findall(r'\{iov_base="((?:\\.|[^"\\])*)", iov_len=(\d+)\}', match[1])
    if len(entries) != int(match[2]): fail('incomplete vector buffer')
    chunks = []
    remaining = n
    for value, length in entries:
        need = min(remaining, int(length))
        data = decode(value)
        if len(data) < need: fail('short captured vector buffer')
        chunks.append(data[:need]); remaining -= need
    if remaining: fail('short vector transfer')
    return b''.join(chunks)


def parse_trace(text, default_pid=None, *, tcp_snapshots=(), session_id=None, evaluation_endpoint=None):
    from origin_observer import order_births
    from origin_descriptors import DescriptorFacts
    descriptors = DescriptorFacts()
    text = order_births(text)
    # Collector-private /proc/net/tcp observations, keyed by socket inode.
    endpoints = {}
    def address(value):
        host, port = value.split(':')
        return '.'.join(str(b) for b in bytes.fromhex(host)[::-1]) + ':' + str(int(port, 16))
    for snapshot in tcp_snapshots:
        for line in snapshot.splitlines()[1:]:
            fields = line.split()
            if len(fields) < 10: fail('malformed TCP table')
            if fields[3] != '01' or fields[9] == '0': continue
            value = address(fields[1]) + '->' + address(fields[2])
            endpoints.setdefault(fields[9], set()).add(value)
    def resolve(annotation):
        if '->' in annotation: return annotation
        values = endpoints.get(annotation, set())
        if len(values) > 1: fail('ambiguous TCP inode endpoint')
        return next(iter(values)) if values else None
    exec_events = []
    stdout_events = []
    stderr_events = []
    pipe_events = []
    pipes = {}
    processes = {}
    pending = {}
    pending_start = {}
    connections = {}
    fds = {}
    cloexec = {}
    serial = 0
    requests, responses = [], []
    events = collections.Counter()
    def proc(pid):
        if pid not in processes:
            processes[pid] = {'pid': pid, 'tgid': pid, 'incarnation': 1, 'exec_path': None, 'exec_argv': None, 'session_id': session_id, 'generation': 0, 'dead': False}
        return processes[pid]
    for sequence, raw in enumerate(text.splitlines(), 1):
        started = sequence
        m = re.match(r'^(?:\[pid\s+(\d+)\]|(\d+))\s+(.*)$', raw)
        if m:
            pid, s = int(m[1] or m[2]), m[3]
        elif default_pid is not None:
            pid, s = default_pid, raw
        else:
            fail('missing PID')
        p = proc(pid)
        if s.endswith('<unfinished ...>'):
            if pid in pending: fail('overlapping unfinished calls')
            pending_start[pid] = sequence
            pending[pid] = s[:-len('<unfinished ...>')]
            continue
        resumed = re.match(r'<\.\.\. (\w+) resumed>(.*)', s)
        if resumed:
            started = pending_start.pop(pid, sequence)
            prefix = pending.pop(pid, None)
            if prefix is None or not prefix.startswith(resumed[1]+'('): fail('unmatched resumed call')
            s = prefix + resumed[2]
        if s.startswith('--- '): continue
        if s.startswith('+++ '):
            p['dead'] = True
            continue
        call = re.match(r'(\w+)\((.*)\)\s+=\s+(.+)$', s)
        if not call: fail('malformed or truncated syscall')
        op, args, result = call.groups()
        if descriptors.observe(pid, op, args, result, unfinished=bool(resumed)):
            fail('unsupported kernel transfer')
        # strace -xx hex-escapes pipe fd annotations on some versions. Decode
        # only the fd prefix (or pipe's fd array), never quoted transfer bytes.
        def pipe_annotation(match):
            annotation = decode(match[1])
            return '<' + annotation.decode('ascii') + '>' if re.fullmatch(rb'pipe:\[\d+\]', annotation) else match[0]
        if op in ('pipe', 'pipe2'):
            end = args.find(']')
            if end >= 0:
                args = re.sub(r'<((?:\\x[0-9a-fA-F]{2})+)>', pipe_annotation, args[:end]) + args[end:]
        else:
            prefix = re.match(r'\d+<((?:\\x[0-9a-fA-F]{2})+)>', args)
            if prefix:
                args = re.sub(r'<((?:\\x[0-9a-fA-F]{2})+)>', pipe_annotation, prefix[0]) + args[prefix.end():]
        events[op] += 1
        if p['dead']:
            fds[pid] = {}
            p = processes[pid] = dict(pid=pid, tgid=pid, incarnation=p['incarnation']+1, exec_path=None, exec_argv=None, session_id=session_id, generation=0, dead=False)
        if op in ('clone', 'clone3', 'fork', 'vfork') and re.match(r'[1-9]\d*$', result):
            child = int(result)
            thread = 'CLONE_THREAD' in args
            shared = 'CLONE_FILES' in args
            if thread and not (shared and 'CLONE_VM' in args): fail('unsupported thread flags')
            table = fds.setdefault(pid, {})
            flags = cloexec.setdefault(pid, set())
            fds[child] = table if shared else table.copy()
            cloexec[child] = flags if shared else flags.copy()
            if child in processes and not processes[child]['dead']: fail('PID lifecycle ambiguity')
            old = processes.get(child)
            p['executable_mappings'] = processes[p['tgid']].setdefault('executable_mappings', [])
            processes[child] = dict(p, pid=child, tgid=p['tgid'] if thread else child,
                                    incarnation=old['incarnation']+1 if old else 1)
            if 'CLONE_VM' not in args and op != 'vfork':
                processes[child]['executable_mappings'] = list(p.get('executable_mappings', []))
        if op in ('execve', 'execveat') and result == '0':
            if p['tgid'] != pid: fail('nonleader exec unsupported')
            # Exec unshares the descriptor table and closes FD_CLOEXEC entries.
            fds[pid] = {fd: key for fd, key in fds.get(pid, {}).items()
                        if fd not in cloexec.get(pid, set())}
            cloexec[pid] = set()
            path = re.match(r'"((?:\\.|[^"\\])*)"', args) if op == 'execve' else None
            if not path: fail('unsupported exec grammar')
            try: p['exec_path'] = decode(path[1]).decode('utf-8', errors='strict')
            except UnicodeError: fail('invalid executable path encoding')
            argv = re.match(r', \[((?:"(?:\\.|[^"\\])*"(?:, )?)*)\],', args[path.end():])
            if not argv: fail('unsupported or truncated exec argv')
            try:
                p['exec_argv'] = [decode(v).decode('utf-8', errors='strict') for v in
                                  re.findall(r'"((?:\\.|[^"\\])*)"', argv[1])]
            except UnicodeError: fail('invalid argv encoding')
            envp = args[path.end():][argv.end():].strip()
            entries = None
            if envp in ('NULL', '0x0'):
                entries = []
            elif re.fullmatch(r'\[(?:"(?:\\.|[^"\\])*"(?:, )?)*\]', envp):
                entries = [decode(v).decode('utf-8', errors='strict') for v in
                           re.findall(r'"((?:\\.|[^"\\])*)"', envp)]
            keys = [entry.split('=', 1)[0] for entry in entries] if entries is not None else []
            p['loader_context_clean'] = (entries is not None and
                all('=' in entry for entry in entries) and len(keys) == len(set(keys)) and
                not any(key.startswith('LD_') or key in ('GLIBC_TUNABLES', 'GCONV_PATH') for key in keys))
            p['executable_mappings'] = []
            p['generation'] += 1
            exec_events.append(dict(p))
        if op in ('mmap', 'mmap2') and not result.startswith('-1') and 'PROT_EXEC' in args:
            mapped = re.search(r', (\d+)<([^>]+)>, [^,]+$', args)
            name = None
            if mapped and 'MAP_ANONYMOUS' not in args and 'PROT_WRITE' not in args:
                try: name = decode(mapped[2]).decode('utf-8')
                except UnicodeError: pass
            processes[p['tgid']].setdefault('executable_mappings', []).append(name)
        if op in ('mprotect', 'pkey_mprotect') and result == '0' and 'PROT_EXEC' in args:
            processes[p['tgid']].setdefault('executable_mappings', []).append(None)
        if op in ('exit', 'exit_group'): p['dead'] = True
        if op in ('pipe', 'pipe2') and result == '0':
            pair = re.match(r'\[\d+<pipe:\[(\d+)\]>, \d+<pipe:\[(\d+)\]>\]', args)
            if not pair or pair[1] != pair[2]: fail('unsupported pipe creation')
            if pair[1] in pipes: fail('reused pipe inode')
            pipes[pair[1]] = started
        pipe = re.match(r'(\d+)<pipe:\[(\d+)\]>, ', args)
        if pipe and op in ('read', 'write', 'readv', 'writev') and result.isdigit() and int(result):
            n = int(result)
            data = transfer_bytes(op, args, n)
            pipe_events.append(dict(pipe=pipe[2], created=pipes.get(pipe[2]), operation=op,
                fd=int(pipe[1]), data_hex=data[:n].hex(), sender=dict(p), started=started,
                completed=sequence))
        if op in ('write', 'writev') and re.match(r'1(?:<[^>]+>)?, ', args) and result.isdigit():
            n = int(result)
            data = transfer_bytes(op, args, n)
            stdout_events.append(dict(data=data[:n].decode('utf-8'), sender=dict(p), started=started, completed=sequence))
        if op in ('write', 'writev') and re.match(r'2(?:<[^>]+>)?, ', args) and result.isdigit():
            n = int(result)
            # Diagnostic stderr may split UTF-8 across writes; keep exact bytes. An
            # uncapturable write only voids stderr attestation, not the whole trace.
            try:
                data_hex = transfer_bytes(op, args, n)[:n].hex()
            except TraceError:
                data_hex = None
            stderr_events.append(dict(data_hex=data_hex, sender=dict(p)))
        if op in ('sendmsg', 'recvmsg', 'sendmmsg', 'recvmmsg') and not result.startswith('-1'):
            controls = re.findall(r'msg_controllen=(\d+)', args)
            if not controls or any(int(n) != 0 for n in controls):
                fail('ancillary socket transfer unsupported')
        # Explicit non-TCP transports cannot contribute to these HTTP receipts.
        if re.match(r'\d+<(?:NETLINK|UNIX(?:-STREAM)?):\[', args): continue
        table = fds.setdefault(pid, {})
        flags = cloexec.setdefault(pid, set())
        endpoint = re.match(r'(\d+)<TCP:\[([^\]]+)\]>', args)
        fd = int(endpoint[1]) if endpoint else None
        if op == 'socket' and 'TCP:[' in result:
            created = re.match(r'(\d+)<TCP:\[([^\]]+)\]>', result)
            if not created: fail('unsupported socket result')
            serial += 1
            created_fd = int(created[1])
            table[created_fd] = (serial, created[2])
            flags.discard(created_fd)
            if 'SOCK_CLOEXEC' in args: flags.add(created_fd)
            continue
        if op in ('dup', 'dup2', 'dup3', 'fcntl'):
            source = re.match(r'(\d+)', args)
            if source and int(source[1]) in table:
                if op == 'fcntl' and 'F_SETFD' in args and result == '0':
                    flags.discard(int(source[1]))
                    if 'FD_CLOEXEC' in args: flags.add(int(source[1]))
                if op == 'fcntl' and 'F_DUPFD' not in args: continue
                dest = re.match(r'(\d+)', result)
                if dest:
                    target = int(dest[1])
                    table[target] = table[int(source[1])]
                    if target != int(source[1]): flags.discard(target)
                    if 'CLOEXEC' in args: flags.add(target)
            continue
        if op == 'close':
            source = re.match(r'(\d+)', args)
            if source and result == '0':
                identity = table.pop(int(source[1]), None)
                flags.discard(int(source[1]))
                if identity and not any(identity in t.values() for t in fds.values()):
                    if identity in connections: connections[identity]['closed'] = True
            continue
        if op == 'ioctl' and result == '0' and ('FIOCLEX' in args or 'FIONCLEX' in args):
            source = re.match(r'(\d+)', args)
            if source:
                flags.discard(int(source[1]))
                if 'FIOCLEX' in args: flags.add(int(source[1]))
            continue
        if op in ('connect','bind','listen','getsockname','getpeername','setsockopt','getsockopt','shutdown','poll','ppoll','select','pselect6','ioctl'):
            continue
        if endpoint:
            if fd not in table:
                if '->' not in endpoint[2]: fail('unknown socket lifecycle')
                if any(c['endpoint'] == endpoint[2] for c in connections.values()):
                    fail('repeated connection identity')
                serial += 1
                table[fd] = (serial, endpoint[2])
            key = table[fd]
        else:
            source = re.match(r'(\d+)', args)
            if source and int(source[1]) in table:
                fd = int(source[1]); key = table[fd]
            else:
                if op in ('sendto','sendmsg','sendmmsg','recvfrom','recvmsg','recvmmsg') and not result.startswith('-1'):
                    fail('socket operation without TCP endpoints')
                continue
        resolved = resolve(endpoint[2] if endpoint else key[1])
        if evaluation_endpoint is not None and (resolved is None or resolved.split('->')[-1] != evaluation_endpoint):
            continue
        if op not in ('write','sendto','read','recvfrom','writev','readv','sendmsg','recvmsg'): fail('unsupported TCP operation')
        if result.startswith('-1'): continue
        if not re.fullmatch(r'\d+',result): fail('unsupported syscall result')
        n = int(result)
        if not n: continue
        data = transfer_bytes(op, args, n)
        image = processes[p['tgid']]
        owner = {k:image[k] for k in ('pid','incarnation','exec_path','exec_argv','session_id','generation')}
        owner.update(loader_context_clean=image.get('loader_context_clean', False),
                     executable_mappings=list(image.get('executable_mappings', [])))
        if key not in connections and resolved is not None and any(
                c['endpoint'] == resolved for c in connections.values()):
            fail('repeated connection identity')
        c = connections.setdefault(key, dict(ordinal=len(connections)+1, owner=owner, fd=fd, closed=False, endpoint=resolve(endpoint[2]) if endpoint else None, tx=bytearray(), rx=bytearray(), tx_events=[], rx_events=[]))
        if c['closed']: fail('repeated connection identity')
        if any(c['owner'][k] != owner[k] for k in owner if k != 'executable_mappings'):
            fail('shared sender or exec ambiguity')
        c['owner']['executable_mappings'] = list(dict.fromkeys(
            c['owner']['executable_mappings'] + owner['executable_mappings']))
        direction = 'tx' if op in ('write','sendto','writev','sendmsg') else 'rx'
        c[direction].extend(data[:n])
        c[direction+'_events'].append(dict(offset=len(c[direction])-n, count=n, sender=owner, started=started, completed=sequence))
    if pending: fail('unfinished syscall at EOF')
    for c in connections.values():
        methods = []
        for direction, output in [('tx',requests),('rx',responses)]:
            data = bytes(c[direction]); pos = 0; ordinal = 0
            while pos < len(data):
                end = data.find(b'\r\n\r\n',pos)
                if end < 0: fail('truncated HTTP headers')
                lines=data[pos:end].split(b'\r\n')
                if not re.fullmatch(b'(?:[A-Z]+ [^ ]+ HTTP/1\\.[01]|HTTP/1\\.[01] [0-9]{3} .*)',lines[0]): fail('unsupported HTTP start line')
                if direction == 'tx':
                    if not re.fullmatch(rb'[A-Z]+ [^ ]+ HTTP/1\.[01]', lines[0]): fail('invalid request direction')
                    method = lines[0].split(b' ')[0].decode('ascii')
                    methods.append(method)
                    status = None
                else:
                    if not re.fullmatch(rb'HTTP/1\.[01] [0-9]{3} .*', lines[0]): fail('invalid response direction')
                    status = int(lines[0].split(b' ')[1]); method = None
                    if ordinal >= len(methods): fail('response without request')
                headers={}
                for h in lines[1:]:
                    if b':' not in h: fail('malformed HTTP header')
                    k,v=h.split(b':',1); k=k.lower()
                    if k in headers: fail('duplicate HTTP header')
                    headers[k]=v.strip()
                length=headers.get(b'content-length',b'')
                if b'transfer-encoding' in headers: fail('unsupported HTTP transfer encoding')
                if direction == 'tx' and not length:
                    length = b'0'  # HTTP requests without either framing header have no body.
                if direction == 'rx' and (status in (204,304) or methods[ordinal] == 'HEAD'):
                    length = b'0'
                if not length.isdigit(): fail('unsupported HTTP framing')
                stop=end+4+int(length)
                if stop>len(data): fail('truncated HTTP body')
                ordinal+=1
                receipt=dict(endpoint=c['endpoint'], connection_ordinal=c['ordinal'], request_ordinal=ordinal, sender=c['owner'], byte_offset=pos, byte_count=stop-pos, body_byte_count=int(length), occurrences=[e for e in c[direction+'_events'] if e['offset']<stop and e['offset']+e['count']>pos])
                receipt['method' if direction == 'tx' else 'status'] = method if direction == 'tx' else status
                receipt['body_sha256'] = hashlib.sha256(data[end+4:stop]).hexdigest()
                # Bodies may contain credentials: expose structure and sizes only.
                try:
                    body=json.loads(data[end+4:stop])
                    receipt['json_object']=isinstance(body,dict)
                    receipt['canonical_json_sha256'] = hashlib.sha256(json.dumps(body, sort_keys=True, separators=(',', ':'), ensure_ascii=True).encode('ascii')).hexdigest()
                    if isinstance(body,dict):
                        params=body.get('params')
                        receipt['tool_call']=body.get('method')=='tools/call'
                        receipt['argument_count']=len(params.get('arguments',{})) if isinstance(params,dict) and isinstance(params.get('arguments',{}),dict) else 0
                except (ValueError,UnicodeError): receipt['json_object']=False
                output.append(receipt); pos=stop
    return dict(requests=requests,responses=responses,event_counts=dict(events), stdout_events=stdout_events, stderr_events=stderr_events, pipe_events=pipe_events, exec_events=exec_events, binary_hash_verified=False)


if __name__ == '__main__':
    ap=argparse.ArgumentParser(); ap.add_argument('trace'); ap.add_argument('--pid',type=int); a=ap.parse_args()
    try:
        r=parse_trace(TraceLines(a.trace),a.pid)
        print(json.dumps({'requests':len(r['requests']),'responses':len(r['responses'])}))
    except (TraceError,StorageError,UnicodeError) as e:
        print(json.dumps({'qualified':False,'diagnostic':str(e) if isinstance(e,(TraceError,StorageError)) else 'invalid encoding'}))
        raise SystemExit(1)
