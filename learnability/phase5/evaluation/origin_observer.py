"""Private collector-started syscall recording. Not an attestation issuer.

The caller supplies the existing isolated-user launch command unchanged. Raw files
contain credentials and must never be published. A completed trace is necessary,
not sufficient, for trustworthy request attribution.
"""
import json
import sys
from trace_storage import TraceLines
import os
import pathlib
import signal
import time
import subprocess
from origin_descriptors import DescriptorFacts, TRACE_FACTS, KERNEL, ASYNC


def tracer_diagnostic(stream):
    """Recognize tracer line prefixes without retaining unbounded client stderr."""
    marker = b'\nstrace:'
    tail = b'\n'
    while chunk := stream.read(65536):
        window = tail + chunk
        if marker in window:
            return True
        tail = window[-(len(marker) - 1):]
    return False


class TraceRun:
    def __init__(self, directory):
        self.directory = pathlib.Path(directory)
        self.directory.mkdir(mode=0o700)
        self.used = False

    def run(self, command, *, timeout, env=None, session_user=None, cwd=None, cleanup=None):
        if self.used:
            raise RuntimeError('trace lifetime already consumed')
        self.used = True
        # -xx makes arbitrary payload bytes unambiguous; -s bounds each buffer.
        args = ['strace', '-f', '-v', '-yy', '-xx', '-s', '1048576', '-o',
                str(self.directory / 'trace.fifo'), '-e',
                'trace=' + TRACE_FACTS + ',%process,%network,pipe,pipe2,read,write,readv,writev,pread64,pwrite64,preadv,pwritev,preadv2,pwritev2,mmap,mprotect,pkey_mprotect,process_vm_writev,close,dup,dup2,dup3,fcntl,unshare,setns,ptrace,' + ','.join(KERNEL + ASYNC),
                '--', *command]
        if session_user is not None:
            args[1:1] = ['-u', session_user]
            if os.geteuid() != 0:
                args = ['sudo', '-n', '--preserve-env=QUEUE_TOKEN,QUEUE_UPGRADE', '--', *args]
        fifo = self.directory / 'trace.fifo'
        os.mkfifo(fifo, 0o600)
        compressor = subprocess.Popen([sys.executable, '-c',
            "import sys,json; from trace_storage import compress_stream; "
            "source=open(sys.argv[1],'rb'); "
            "result=compress_stream(source,sys.argv[2]); "
            "print(json.dumps(result))", str(fifo), str(self.directory / 'trace')],
            cwd=str(pathlib.Path(__file__).resolve().parent), stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, start_new_session=True)
        timed_out = False
        child = None
        def stop_tracer():
            if child is None or child.poll() is not None:
                return
            # The tracer may run as root even when the collector does not.
            for sig in (signal.SIGTERM, signal.SIGKILL):
                if session_user is not None and os.geteuid() != 0:
                    subprocess.run(['sudo', '-n', 'kill', '-' + str(int(sig)), '--',
                                    '-' + str(child.pid)], check=True,
                                   capture_output=True, timeout=10)
                else:
                    try:
                        os.killpg(child.pid, sig)
                    except ProcessLookupError:
                        pass
                try:
                    child.wait(timeout=5)
                    return
                except subprocess.TimeoutExpired:
                    continue
            raise RuntimeError('tracer did not terminate')
        try:
            with open(self.directory / 'stdout', 'xb') as out, open(
                    self.directory / 'stderr', 'xb') as err:
                os.chmod(out.name, 0o600)
                os.chmod(err.name, 0o600)
                child = subprocess.Popen(args, stdout=out, stderr=err, env=env, cwd=cwd,
                                         start_new_session=True)
                deadline = time.monotonic() + timeout
                while child.poll() is None:
                    if compressor.poll() is not None:
                        # EOF can precede the tracer's final exit by a few scheduler turns.
                        try:
                            child.wait(timeout=1)
                        except subprocess.TimeoutExpired:
                            raise RuntimeError('trace compressor exited while tracer active')
                        break
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        timed_out = True
                        try:
                            if cleanup is not None:
                                cleanup()
                        finally:
                            stop_tracer()
                        break
                    try:
                        child.wait(timeout=min(0.1, remaining))
                    except subprocess.TimeoutExpired:
                        pass
            metadata, _ = compressor.communicate(timeout=10)
            if compressor.returncode != 0:
                raise RuntimeError('trace compression failed')
            decoded_pin = json.loads(metadata)
        finally:
            try:
                stop_tracer()
            finally:
                if compressor.poll() is None:
                    compressor.kill()
                compressor.wait(timeout=10)
                compressor.stdout.close()
                fifo.unlink(missing_ok=True)
        path = self.directory / 'trace'
        lines = TraceLines(path)
        complete = lines.endswith('\n')
        integrity = process_integrity(lines)
        complete = complete and integrity['process_completeness_verified']
        with (self.directory / 'stderr').open('rb') as stderr:
            tracer_errors = tracer_diagnostic(stderr)
        if tracer_errors:
            integrity['integrity_errors'].append('tracer diagnostic')
            integrity['process_completeness_verified'] = False
            complete = False
        return {'decoded_trace': decoded_pin, 'trace_storage': 'gzip', 'returncode': child.returncode, 'timed_out': timed_out,
                'capture_finished': complete and not timed_out and child.returncode == 0,
                **integrity,
                'qualified': False}


def order_births(text):
    """Place a paired birth at syscall entry, before the child can execute.

    Only exact per-PID unfinished/resumed fork calls are moved. This establishes
    lineage, not wall-clock or cross-channel observation ordering.
    """
    import re
    pending, replacements, removed = {}, {}, set()
    for index, line in enumerate(text.splitlines()):
        start = re.fullmatch(r'(\d+)\s+((?:clone3?|vfork|fork)\(.*)<unfinished \.\.\.>', line)
        if start:
            if start[1] in pending:
                continue
            pending[start[1]] = (index, start[2])
        end = re.fullmatch(r'(\d+)\s+<\.\.\. (clone3?|vfork|fork) resumed>(.*)', line)
        if end and end[1] in pending:
            previous, prefix = pending.pop(end[1])
            if prefix.startswith(end[2] + '('):
                replacements[previous] = end[1] + ' ' + prefix + end[3]
                removed.add(index)
    class Ordered:
        def splitlines(self):
            for index, line in enumerate(text.splitlines()):
                if index not in removed:
                    yield replacements.get(index, line)
    ordered = Ordered()
    if isinstance(text, str):
        return '\n'.join(ordered.splitlines()) + ('\n' if text.endswith('\n') else '')
    return ordered


def process_integrity(text):
    """Conservative strace launch ledger; unsupported ordering fails closed."""
    import re
    ledger, pending, errors = {}, {}, []
    descriptors = DescriptorFacts()
    root = None
    for line in order_births(text).splitlines():
        match = re.fullmatch(r'(\d+)\s+(.*)', line)
        if not match:
            errors.append('unparsed trace record'); continue
        pid, call = int(match[1]), match[2]
        if root is None:
            root = pid
            ledger[pid] = dict(pid=pid, incarnation=1, attached_before_exec=call.startswith('execve('), terminal=False)
        if pid not in ledger:
            ledger[pid] = dict(pid=pid, incarnation=1, attached_before_exec=False, terminal=False)
            errors.append('unregistered descendant')
        entry = ledger[pid]
        if entry['terminal']:
            errors.append('PID reuse unsupported')
        if call.endswith('<unfinished ...>'):
            if pid in pending: errors.append('overlapping calls')
            pending[pid] = call[:-16]; continue
        resumed = re.match(r'<\.\.\. (\w+) resumed>(.*)', call)
        if resumed:
            prefix = pending.pop(pid, '')
            if not prefix.startswith(resumed[1] + '('): errors.append('unmatched resume')
            call = prefix + resumed[2]
        if re.fullmatch(r'\+\+\+ (?:exited with \d+|killed by SIG\w+(?: \(core dumped\))?) \+\+\+', call):
            entry['terminal'] = True; continue
        if call.startswith('--- '): continue
        syscall = re.fullmatch(r'(\w+)\((.*)\)\s+=\s+(.+)', call)
        if not syscall:
            errors.append('unparsed syscall'); continue
        op, args, result = syscall.groups()
        if descriptors.observe(pid, op, args, result, unfinished=bool(resumed)):
            errors.append('unsupported kernel transfer')
        if op in ('clone', 'clone3', 'fork', 'vfork') and result.isdigit() and int(result) > 0:
            child = int(result)
            if child in ledger: errors.append('ambiguous child ordering')
            else: ledger[child] = dict(pid=child, incarnation=1, attached_before_exec=True, terminal=False)
        if op in ('unshare', 'setns', 'ptrace', 'process_vm_writev') and re.match(r'^\d+', result):
            errors.append('unsupported isolation or memory transition')
        if op in ('write', 'writev', 'pwrite64', 'pwritev', 'pwritev2') and re.match(r'^[1-9]\d*', result):
            descriptor = re.match(r'\d+<([^>]+)>', args)
            if descriptor:
                from origin_trace import decode
                name = decode(descriptor[1])
                if re.fullmatch(rb'/proc/(?:\d+|self|thread-self)/(?:task/\d+/)?mem', name):
                    errors.append('unsupported process memory write')
    if pending: errors.append('unfinished syscall at EOF')
    complete = bool(ledger) and text.endswith('\n') and not errors and all(
        p['attached_before_exec'] and p['terminal'] for p in ledger.values())
    return dict(process_completeness_verified=complete, processes=list(ledger.values()), integrity_errors=sorted(set(errors)))


def qualification(facts):
    """Internal conjunction only; callers must supply collector-established facts."""
    required = ('process_completeness_verified', 'capture_finished', 'identity_bound',
                'session_consumed_once', 'socket_lifecycle_verified', 'requests_reconciled',
                'backend_denial_verified', 'cleanup_verified')
    missing = [key for key in required if facts.get(key) is not True]
    return dict(qualified=not missing, missing=missing, scope='bounded scalar TCP HTTP/1 Content-Length')
