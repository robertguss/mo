#!/usr/bin/env python3
"""Measure the largest service state an export must carry, against today's mo serve.

  python3 learnability/phase4/preflight/sizing.py MO queue [JOBS [PAYLOAD_BYTES]]
        fill the task queue with JOBS jobs (default 5000, its limit) of PAYLOAD_BYTES bytes
        each (default 2048, its limit), lease a tenth of them, then stop it
  python3 learnability/phase4/preflight/sizing.py MO grow [KIB_STEP]
        grow the boundary fixture's state by KIB_STEP KiB per call (default 1024) until the
        runtime refuses the next state, then stop it

Both print one JSON object: the encoded snapshot and log sizes read from the stopped data
folder, the database file size, the process's peak and final resident memory, the latency of
the last updating calls and of a read-only call at the largest state, and how the run ended.
The token is a fixed canary that names itself; nothing here is a credential.
"""

import http.client
import json
import os
import pathlib
import select
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent.parent
TOKEN = 'Phase4SizingCanary'
PROTOCOL = '2025-11-25'


class Refused(Exception):
    """mo serve exited before it was listening."""

    def __init__(self, status, stderr):
        super().__init__(f'exited {status}: {stderr}')
        self.status, self.stderr = status, stderr


class Server:
    def __init__(self, binary, program, token_env, data):
        self.process = subprocess.Popen(
            [str(binary), 'serve', str(program), '--port', '0', '--data', str(data)],
            env={**os.environ, token_env: TOKEN}, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, bufsize=0, cwd=ROOT)
        try:
            self.port = self.listening()
        except RuntimeError:
            self.process.kill()
            self.process.wait(timeout=60)
            raise
        self.next_id = 0
        self.peak_kib = 0

    def listening(self):
        line = b''
        limit = time.monotonic() + 60
        while not line.endswith(b'\n'):
            if not select.select([self.process.stdout], [], [], limit - time.monotonic())[0]:
                raise RuntimeError(f'no listening line: {line!r}')
            chunk = self.process.stdout.read(1)
            if not chunk:
                status = self.process.wait(timeout=60)
                stderr = self.process.stderr.read().decode('utf-8', 'replace').strip()
                self.process.stdout.close()
                self.process.stderr.close()
                raise Refused(status, stderr)
            line += chunk
        return int(line.split(b':')[-1].split(b'/')[0])

    def memory(self):
        """Peak and current resident memory in KiB, from /proc."""
        fields = {}
        with open(f'/proc/{self.process.pid}/status', encoding='ascii') as status:
            for line in status:
                name, _, value = line.partition(':')
                fields[name] = value.strip()
        return int(fields['VmHWM'].split()[0]), int(fields['VmRSS'].split()[0])

    def call(self, tool, **arguments):
        self.next_id += 1
        body = json.dumps({'jsonrpc': '2.0', 'id': self.next_id, 'method': 'tools/call',
                           'params': {'name': tool, 'arguments': arguments}})
        # Stay under the token's rate limit of 20 calls a second.
        time.sleep(0.052)
        started = time.monotonic()
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=60)
        connection.request('POST', '/mcp', body, {
            'Authorization': f'Bearer {TOKEN}', 'Content-Type': 'application/json',
            'MCP-Protocol-Version': PROTOCOL})
        response = connection.getresponse()
        data = response.read()
        connection.close()
        elapsed = time.monotonic() - started
        if response.status != 200:
            raise RuntimeError(f'{tool}: HTTP {response.status}: {data[:200]!r}')
        result = json.loads(data)['result']
        return result['structuredContent'], result['isError'], elapsed, len(data)

    def stop(self):
        self.peak_kib, final_kib = self.memory()
        self.process.send_signal(signal.SIGTERM)
        status = self.process.wait(timeout=60)
        stderr = self.process.stderr.read().decode('utf-8', 'replace')
        self.process.stdout.close()
        self.process.stderr.close()
        return status, stderr, final_kib


def store_sizes(data):
    path = data / 'service.sqlite'
    connection = sqlite3.connect(f'file:{path}?mode=ro', uri=True)
    try:
        snapshot = connection.execute('SELECT length(record) FROM snapshot').fetchone()[0]
        logs = [row[0] for row in connection.execute('SELECT length(record) FROM log')]
        head = connection.execute('SELECT seq FROM head').fetchone()[0]
    finally:
        connection.close()
    return {'snapshot_record_bytes': snapshot, 'log_records': len(logs),
            'log_record_bytes_max': max(logs, default=0), 'log_record_bytes_total': sum(logs),
            'head_sequence': head, 'database_file_bytes': path.stat().st_size}


def error_code(content, failed):
    """A failed call's error code, or None when it succeeded."""
    return content['error'].get('code', content['error'].get('kind')) if failed else None


def fill_queue(server, jobs, payload_bytes):
    payload = 'q' * payload_bytes
    latencies = []
    ended = 'filled'
    for n in range(jobs):
        content, failed, elapsed, _ = server.call('submit', payload=payload)
        latencies.append(elapsed)
        if failed:
            ended = f'refused at job {n + 1}: {json.dumps(content)[:200]}'
            break
    leased = 0
    for _ in range(jobs // 10):
        content, failed, elapsed, _ = server.call('lease')
        latencies.append(elapsed)
        if failed:
            break
        leased += 1
    stats, stats_failed, stats_seconds, stats_bytes = server.call('stats')
    listed, list_failed, list_seconds, list_bytes = server.call(
        'list', filter={'case': 'AllJobs'}, after='0', limit='10')
    return {'jobs_submitted': n + 1 if ended != 'filled' else jobs, 'jobs_leased': leased,
            'payload_bytes': payload_bytes, 'ended': ended,
            'last_update_seconds': latencies[-20:], 'stats_seconds': stats_seconds,
            'stats_response_bytes': stats_bytes, 'stats_error': error_code(stats, stats_failed),
            'list_10_seconds': list_seconds, 'list_10_response_bytes': list_bytes,
            'list_10_error': error_code(listed, list_failed)}


def grow_state(server, step):
    latencies = []
    calls = 0
    ended = 'unexpected end'
    while calls < 400:
        content, failed, elapsed, _ = server.call('grow', kib=str(step))
        latencies.append(elapsed)
        if failed:
            ended = f'refused at call {calls + 1}: {json.dumps(content)[:300]}'
            break
        calls += 1
    content, _, size_seconds, _ = server.call('size')
    return {'step_kib': step, 'accepted_calls': calls, 'held': content, 'ended': ended,
            'last_update_seconds': latencies[-5:], 'size_seconds': size_seconds}


def main(args):
    if len(args) < 2 or args[1] not in ('queue', 'grow'):
        raise SystemExit(__doc__)
    binary = pathlib.Path(args[0]).resolve()
    with tempfile.TemporaryDirectory() as folder:
        data = pathlib.Path(folder)
        if args[1] == 'queue':
            server = Server(binary, 'services/task-queue/main.mo', 'QUEUE_TOKEN', data)
            jobs = int(args[2]) if len(args) > 2 else 5000
            payload = int(args[3]) if len(args) > 3 else 2048
            result = fill_queue(server, jobs, payload)
        else:
            server = Server(binary, HERE / 'grow' / 'main.mo', 'GROW_TOKEN', data)
            result = grow_state(server, int(args[2]) if len(args) > 2 else 1024)
        status, stderr, final_kib = server.stop()
        result.update(store_sizes(data))
        result.update({'peak_resident_kib': server.peak_kib, 'final_resident_kib': final_kib,
                       'exit_status': status, 'stderr': stderr[-2000:]})
    print(json.dumps(result, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
