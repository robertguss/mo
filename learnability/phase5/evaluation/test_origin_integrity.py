"""Bounded local controls; raw artifacts retained under /root, never scored."""
import json
import os
from pathlib import Path
import pwd
import subprocess
import sys
import tempfile
import unittest
import origin_observer as observer
from trace_storage import TraceLines
import origin_trace


class IntegrityTests(unittest.TestCase):
    def test_missing_never_true(self):
        self.assertFalse(observer.qualification({})['qualified'])
        self.assertFalse(observer.process_integrity('1 +++ exited with 0 +++\n')['process_completeness_verified'])

    def test_truncated_child(self):
        text = '1 execve("/bin/true", [], []) = 0\n1 fork() = 2\n1 +++ exited with 0 +++\n'
        self.assertFalse(observer.process_integrity(text)['process_completeness_verified'])
        self.assertTrue(observer.process_integrity(text + '2 +++ exited with 0 +++\n')['process_completeness_verified'])
        self.assertFalse(observer.process_integrity(text + '2 read(3, <unfinished ...>\n')['process_completeness_verified'])

    def test_namespace_creation_through_clone_flags_is_refused(self):
        plain = '1 execve("/bin/true", [], []) = 0\n1 clone(child_stack=NULL, flags=SIGCHLD) = 2\n2 +++ exited with 0 +++\n1 +++ exited with 0 +++\n'
        self.assertTrue(observer.process_integrity(plain)['process_completeness_verified'])
        for call in ('clone(child_stack=NULL, flags=CLONE_NEWNS|CLONE_NEWUSER|SIGCHLD) = 2',
                     'clone3({flags=CLONE_NEWPID, exit_signal=SIGCHLD}, 88) = 2',
                     'clone(child_stack=NULL, <unfinished ...>\n1 <... clone resumed>flags=CLONE_NEWNS|SIGCHLD) = 2'):
            with self.subTest(call=call):
                result = observer.process_integrity(plain.replace('clone(child_stack=NULL, flags=SIGCHLD) = 2', call))
                self.assertFalse(result['process_completeness_verified'])
                self.assertIn('unsupported isolation or memory transition', result['integrity_errors'])



def live():
    import threading
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
    import proxy
    assert os.geteuid() == 0
    os.umask(0o077)
    user = pwd.getpwnam('mo-phase5-session')
    assert user.pw_uid not in (0, os.getuid())
    def remaining():
        return subprocess.run(['pgrep', '-U', str(user.pw_uid)], capture_output=True).stdout.split()
    assert not remaining(), 'dedicated account must start empty'
    root = Path(tempfile.mkdtemp(prefix='mo-origin-integrity-', dir='/root'))
    results = []
    baseline = subprocess.check_output(['iptables', '-S', 'OUTPUT'])
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200); self.send_header('Content-Length', '2'); self.end_headers(); self.wfile.write(b'ok')
        def log_message(self, *args): pass
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever); thread.start()
    port = server.server_port
    recorder = proxy.Proxy(f'http://127.0.0.1:{port}/', root / 'proxy.jsonl', 'development-token', 'development-upgrade')
    rule = ['OUTPUT', '-p', 'tcp', '--dport', str(port), '-m', 'owner', '--uid-owner', str(user.pw_uid), '-j', 'REJECT']
    installed = False
    try:
        cases = {
            'normal': 'pass',
            'fork_dup': f'import socket,os\ns=socket.create_connection(("127.0.0.1",{port})); d=os.dup(s.fileno()); p=os.fork()\nif p: os.waitpid(p,0)\nelse: os.write(d,b"GET / HTTP/1.0\\r\\n\\r\\n"); os._exit(0)',
            'two_senders': f'import socket,os\ns=socket.create_connection(("127.0.0.1",{port})); p=os.fork()\ns.sendall(b"GET / HTTP/1.0\\r\\n\\r\\n")\nif p: os.waitpid(p,0)\nelse: os._exit(0)',
            'daemon': 'import os,time\nif os.fork(): os._exit(0)\nos.setsid()\nif os.fork(): os._exit(0)\ntime.sleep(.1)',
            'escaped_timeout': 'import os,time\nif os.fork(): os._exit(0)\nos.setsid()\ntime.sleep(30)',
        }
        for name, code in cases.items():
            run = observer.TraceRun(root / name).run([sys.executable, '-c', code], timeout=1 if name == 'escaped_timeout' else 10, session_user=user.pw_name, cwd='/tmp')
            rejected = False
            try: origin_trace.parse_trace(TraceLines(root / name / 'trace'))
            except origin_trace.TraceError: rejected = True
            if name in ('fork_dup', 'two_senders'): passed = rejected
            elif name == 'escaped_timeout': passed = run['timed_out'] and not run['capture_finished']
            else: passed = run['capture_finished'] or not run['process_completeness_verified']
            # Existing protected harness end_all pattern: dedicated UID only.
            import time
            for _ in range(80):
                if not remaining(): break
                subprocess.run(['pkill', '-KILL', '-U', str(user.pw_uid)], capture_output=True)
                time.sleep(.25)
            results.append(dict(case=name, passed=passed, run=run, ambiguity_rejected=rejected, cleanup_verified=not remaining()))
        subprocess.run(['iptables', '-I', *rule], check=True); installed = True
        def curl(url):
            return subprocess.run(['runuser', '-u', user.pw_name, '--', 'curl', '--noproxy', '*', '-fsS', '--max-time', '3', url], capture_output=True)
        direct = curl(f'http://127.0.0.1:{port}/')
        via = curl(recorder.url)
        results.append(dict(case='backend_denial', passed=direct.returncode != 0 and via.returncode == 0, direct_returncode=direct.returncode, proxy_returncode=via.returncode))
    finally:
        if installed: subprocess.run(['iptables', '-D', *rule], check=True)
        recorder.stop(); server.shutdown(); server.server_close(); thread.join()
        for _ in range(80):
            if not remaining(): break
            subprocess.run(['pkill', '-KILL', '-U', str(user.pw_uid)], capture_output=True)
            __import__('time').sleep(.25)
        restored = subprocess.check_output(['iptables', '-S', 'OUTPUT']) == baseline
        summary = dict(results=results, firewall_restored=restored, uid_empty=not remaining(), qualified=False)
        (root / 'results.json').write_text(json.dumps(summary, indent=2))
    print(json.dumps(dict(private_root=str(root), **summary)))
    return all(r['passed'] for r in results) and restored and not remaining()


if __name__ == '__main__':
    if '--live' in sys.argv: raise SystemExit(0 if live() else 1)
    unittest.main()
