"""Opt-in real controls: python3 test_origin_live.py. Private artifacts retained."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import sys
import tempfile
import pwd
import origin_identity

import fixture
import origin_join
import origin_trace
from origin_observer import TraceRun
import proxy

MO = Path(__file__).resolve().parents[3] / 'build/gcc/debug/mo'
HASH = '7c3c05e122e41159eb27dd099bda276df408d998dfac96146d5c42743d20d182'


def run_controls():
    assert shutil.which('strace') and shutil.which('curl')
    assert hashlib.sha256(MO.read_bytes()).hexdigest() == HASH
    assert os.geteuid() == 0, 'run with sudo -n python3'
    os.umask(0o077)
    account = pwd.getpwnam('mo-phase5-session')
    root = Path(tempfile.mkdtemp(prefix='mo-origin-live-', dir='/root'))
    install = Path(tempfile.mkdtemp(prefix='mo-origin-install-', dir='/opt'))
    install.chmod(0o755)
    shutil.copytree(MO.parents[3] / 'lib', install / 'lib')
    for path in [install / 'lib', *(install / 'lib').rglob('*')]:
        path.chmod(0o755 if path.is_dir() else 0o444)
    binary = install / 'mo'
    shutil.copyfile(MO, binary)
    binary.chmod(0o555)
    assert hashlib.sha256(binary.read_bytes()).hexdigest() == HASH
    root.chmod(0o700)
    results = []
    for name in ('curl', 'urllib', 'push'):
        folder = root / name
        folder.mkdir(mode=0o700)
        token, upgrade = secrets.token_hex(24), secrets.token_hex(24)
        old = fixture.sources(folder / 'old', 'unchanged')
        candidate = fixture.sources(install / ('candidate-' + name))
        for path in [candidate, *candidate.rglob('*')]:
            path.chmod(0o755 if path.is_dir() else 0o444)
        service = fixture.Service(MO, folder / 'data', token, upgrade,
                                  folder / 'service.log', old / 'main.mo')
        recorder = proxy.Proxy(service.url, folder / 'proxy.jsonl', token, upgrade)
        snapshots = []
        original = recorder.recorder.write
        def write(entry):
            if entry['direction'] == 'request':
                snapshots.append(Path('/proc/net/tcp').read_text())
            return original(entry)
        recorder.recorder.write = write
        env = {**os.environ, 'QUEUE_TOKEN': token, 'QUEUE_UPGRADE': upgrade,
               'LIVE_URL': recorder.url}
        body = json.dumps({'jsonrpc': '2.0', 'id': 1, 'method': 'tools/call',
                           'params': {'name': 'stats', 'arguments': {}}})
        curl = ['curl', '-sS', '--noproxy', '*', '-H', 'Content-Type: application/json', '-H', 'MCP-Protocol-Version: 2025-11-25',
                '-H', 'Authorization: Bearer ' + token, '--data-binary', body, recorder.url]
        command = curl
        if name == 'urllib':
            command = [sys.executable, '-c', '''import os,json,urllib.request
for _ in range(2):
 b=json.dumps(dict(jsonrpc='2.0',id=1,method='/'.join(['tools','call']),params=dict(name=''.join(['st','ats']),arguments={}))).encode()
 r=urllib.request.Request(os.environ['LIVE_URL'],data=b,headers={'MCP-Protocol-Version':'2025-11-25','Content-Type':'application/json','Authorization':'Bearer '+os.environ['QUEUE_TOKEN']})
 with urllib.request.urlopen(r) as response: response.read()
''']
        runs = []
        captures = {}
        parts = ['primary', 'unrelated'] if name == 'push' else ['primary']
        for part in parts:
            captures[part] = origin_identity.Capture(folder / (part + '-identity'), name + '-' + part, account.pw_uid, binary)
        try:
            if name == 'push':
                checked = subprocess.run([str(MO), 'check', str(candidate / 'main.mo'),
                    '--upgrade-from', str(old / 'main.mo'), '--migrate', 'migrate', '--json'],
                    capture_output=True, timeout=120, check=True)
                identity = json.loads(checked.stdout)
                command = [str(binary), 'push', str(candidate / 'main.mo'), '--url', recorder.url,
                    '--base', identity['base_source'], '--migrate', 'migrate',
                    '--token-env', 'QUEUE_TOKEN', '--upgrade-token-env', 'QUEUE_UPGRADE', '--json']
            with concurrent.futures.ThreadPoolExecutor() as pool:
                jobs = [pool.submit(TraceRun(folder / 'primary').run, command, timeout=180, env=env, session_user=account.pw_name, cwd=install)]
                if name == 'push':
                    jobs.append(pool.submit(TraceRun(folder / 'unrelated').run, curl, timeout=180, env=env, session_user=account.pw_name, cwd=install))
                runs = [job.result() for job in jobs]
        finally:
            recorder.stop()
            service.stop()
        (folder / 'tcp.json').write_text(json.dumps(snapshots))
        summary = dict(case=name, runs=runs, qualified=False)
        try:
            assert all(r['capture_finished'] for r in runs), 'command or capture failed'
            rows = [json.loads(line) for line in (folder / 'proxy.jsonl').read_text().splitlines()]
            traces = []
            for part in parts:
                capture = captures[part]
                trace = folder / part / 'trace'
                snapshot = folder / (part + '-tcp.json')
                snapshot.write_text(json.dumps(dict(session=capture.session, nonce=capture.nonce,
                    trace_sha256=origin_identity.protected(trace, os.geteuid(), account.pw_uid, private=True, pin_only=True)[0]['sha256'], snapshots=snapshots)))
                capture.seal(trace, folder / 'proxy.jsonl', snapshot)
                traces.append(capture.consume(capture.session)['trace'])
            summary['requests'] = sum(len(t['requests']) for t in traces)
            summary['responses'] = sum(len(t['responses']) for t in traces)
            summary['proxy_requests'] = sum(r['direction'] == 'request' for r in rows)
            combined = {'requests': [], 'responses': []}
            for i,t in enumerate(traces):
                for direction in combined:
                    for receipt in t[direction]:
                        receipt['connection_ordinal'] += i * 100000
                        combined[direction].append(receipt)
            assert all(r['status'] == 200 for r in rows if r['direction'] == 'response')
            joined = origin_join.join(combined, rows)
            assert len(joined['observations']) == summary['requests'] == summary['responses'] == summary['proxy_requests']
            assert not joined['qualified'] and not joined['command_origin']
            if name != 'push': assert summary['requests'] == (1 if name == 'curl' else 2)
            else:
                report = json.loads((folder / 'primary/stdout').read_text())
                (folder / 'accepted-report.json').write_text(json.dumps(report))
                summary['accepted_ordinal'] = report['ordinal']
                assert report['ordinal'] == '1'
                assert summary['requests'] == 7
                assert len(traces[0]['requests']) == 6
                assert len(traces[1]['requests']) == 1
                assert {r['sender']['exec_path'] for r in traces[0]['requests']} == {str(binary)}
                assert all(r['sender']['exec_path'] != str(binary) for r in traces[1]['requests'])
            summary['verified_push'] = sum(r['verified_push_identity'] for t in traces for r in t['requests'])
            assert summary['verified_push'] == (6 if name == 'push' else 0)
            summary['verified_nonpush'] = summary['requests'] - summary['verified_push']
            summary['observations'] = len(joined['observations'])
            summary['pass'] = True
        except (AssertionError, ValueError) as exc:
            summary['pass'] = False
            summary['reason'] = str(exc)
        results.append(summary)
    (root / 'results.json').write_text(json.dumps(results, indent=2))
    shutil.rmtree(install)
    print(json.dumps({'private_root': str(root), 'results': results}, indent=2))
    return results


if __name__ == '__main__':
    # The frozen build embeds its checkout library path. Temporarily permit traversal.
    home = Path('/home/exedev')
    mode = home.stat().st_mode & 0o777
    try:
        home.chmod(mode | 0o001)
        raise SystemExit(0 if all(r['pass'] for r in run_controls()) else 1)
    finally:
        home.chmod(mode)
