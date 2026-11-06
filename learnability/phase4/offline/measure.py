#!/usr/bin/env python3
"""Measure the store reader behind `mo history`: its lock, its memory, its render time, and
what a reader does to a `mo serve` that starts while it holds the store.

  python3 learnability/phase4/offline/measure.py build MO OUT
        fill the task queue under MO with 2,048-byte jobs, stopping it with SIGTERM at 300,
        500, 1,000, 2,000, 4,000 and 5,000 jobs (each a multiple of the snapshot interval, so
        the snapshot holds the whole state) and copying its data folder to OUT/jobs-N; after
        5,000 it leases a tenth of them, as preflight/sizing.py does, into OUT/jobs-5000
  python3 learnability/phase4/offline/measure.py tail MO OUT
        a service whose updating tool takes one near-limit Text argument, called until the
        log tail is as long as it gets; the stopped folder goes to OUT/tail
  python3 learnability/phase4/offline/measure.py reader MO FOLDER...
        for each folder, run `mo history --json FOLDER`: the time from the reader's first
        lock to its last unlock (strace's timestamps on fcntl), the peak resident memory and
        wall time (GNU time), and the rendered bytes
  python3 learnability/phase4/offline/measure.py serve MO FOLDER [SECONDS]
        hold a shared lock on a copy of FOLDER's store for SECONDS (default 3) from Python's
        sqlite3, start `mo serve` on it meanwhile, and time its listening line; then the same
        with `mo history` on the copy started just before the serve

Each prints one JSON object per measurement. Nothing here is a credential.
"""

import json
import os
import pathlib
import re
import shutil
import signal
import sqlite3
import struct
import subprocess
import sys
import tempfile
import threading
import time

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent.parent
sys.path.insert(0, str(HERE.parent / 'preflight'))
import sizing  # noqa: E402

CHECKPOINTS = (300, 500, 1000, 2000, 4000, 5000)
TAIL = '''module Main
service
  state Int = 0
  tools put
  token from env "TAIL_TOKEN"
end
fn put(state: Int, _now: Time, text: Text) : (Int, Int)
  (state + 1, text.byte_count)
end
'''
# The largest Text whose tools/call body fits MO_LIMIT_MCP_BODY_BYTES (16 KiB) with its envelope.
TAIL_TEXT_BYTES = 16384 - 160


def store(data):
    path = pathlib.Path(data) / 'service.sqlite'
    connection = sqlite3.connect(f'file:{path}?mode=ro', uri=True)
    try:
        (record,) = connection.execute('SELECT record FROM snapshot').fetchone()
        logs = [row[0] for row in connection.execute('SELECT length(record) FROM log')]
    finally:
        connection.close()
    # The frame's sequence follows its magic and kind, as fixture.py's log_records reads it.
    return {'snapshot_sequence': struct.unpack('<Q', record[6:14])[0],
            'snapshot_record_bytes': len(record),
            'log_rows': len(logs), 'log_row_bytes_max': max(logs, default=0),
            'database_file_bytes': path.stat().st_size}


def build(binary, out):
    out.mkdir(parents=True, exist_ok=False)
    data = out / 'serving'
    data.mkdir()
    payload = 'q' * 2048
    jobs = 0
    for checkpoint in CHECKPOINTS:
        server = sizing.Server(binary, 'services/task-queue/main.mo', 'QUEUE_TOKEN', data)
        try:
            while jobs < checkpoint:
                content, failed, _, _ = server.call('submit', payload=payload)
                if failed:
                    raise SystemExit(f'submit {jobs + 1} failed: {content}')
                jobs += 1
            if checkpoint == CHECKPOINTS[-1]:
                for _ in range(checkpoint // 10):
                    content, failed, _, _ = server.call('lease')
                    if failed:
                        raise SystemExit(f'lease failed: {content}')
        finally:
            status, stderr, _ = server.stop()
        if status != 0:
            raise SystemExit(f'mo serve exited {status}: {stderr[-500:]}')
        shutil.copytree(data, out / f'jobs-{checkpoint}')
        print(json.dumps({'folder': str(out / f'jobs-{checkpoint}'), 'jobs': checkpoint,
                          **store(data)}), flush=True)
    shutil.rmtree(data)


def tail(binary, out):
    folder = out / 'tail'
    folder.mkdir(parents=True, exist_ok=False)
    program = out / 'tail-program' / 'main.mo'
    program.parent.mkdir()
    program.write_text(TAIL)
    text = 't' * TAIL_TEXT_BYTES
    server = sizing.Server(binary, program, 'TAIL_TOKEN', folder)
    try:
        for n in range(99):  # a snapshot every 100 messages empties the tail
            content, failed, _, size = server.call('put', text=text)
            if failed:
                raise SystemExit(f'put {n + 1} failed: {content}')
    finally:
        status, stderr, _ = server.stop()
    if status != 0:
        raise SystemExit(f'mo serve exited {status}: {stderr[-500:]}')
    print(json.dumps({'folder': str(folder), 'text_bytes': TAIL_TEXT_BYTES, **store(folder)}))


LOCK = re.compile(r'^(?:\d+ +)?(\d+):(\d+):(\d+\.\d+) fcntl\(\d+, F_(?:OFD_)?SETLK, \{l_type=(F_\w+)')


def seconds(match):
    hours, minutes, rest = int(match[1]), int(match[2]), float(match[3])
    return hours * 3600 + minutes * 60 + rest


def reader(binary, folder):
    with tempfile.TemporaryDirectory() as scratch:
        trace = pathlib.Path(scratch) / 'trace'
        timing = pathlib.Path(scratch) / 'time'
        command = ['/usr/bin/time', '-v', '-o', str(timing), 'strace', '-f', '-tt',
                   '-e', 'trace=fcntl', '-o', str(trace), str(binary), 'history', '--json',
                   str(folder)]
        started = time.monotonic()
        result = subprocess.run(command, capture_output=True, timeout=3600, check=False)
        wall = time.monotonic() - started
        traced = trace.read_text()
        locks = [m for m in map(LOCK.match, traced.splitlines()) if m]
        if not locks:
            raise SystemExit(f'no lock in the trace of {folder}: {result.stderr[-500:]!r} '
                             f'{traced[:1000]!r}')
        first = next(m for m in locks if m[4] in ('F_RDLCK', 'F_WRLCK'))
        last = [m for m in locks if m[4] == 'F_UNLCK'][-1]
        peak = re.search(r'Maximum resident set size \(kbytes\): (\d+)', timing.read_text())
        plain_started = time.monotonic()
        plain = subprocess.run([str(binary), 'history', '--json', str(folder)],
                               capture_output=True, timeout=3600, check=False)
        plain_wall = time.monotonic() - plain_started
    return {'folder': str(folder), 'status': result.returncode, **store(folder),
            'lock_seconds': round(seconds(last) - seconds(first), 4),
            'lock_operations': len(locks), 'peak_resident_kib': int(peak[1]),
            'traced_wall_seconds': round(wall, 2), 'wall_seconds': round(plain_wall, 2),
            'plain_status': plain.returncode, 'output_bytes': len(plain.stdout)}


def listening_after(process):
    line = b''
    while not line.endswith(b'\n'):
        chunk = process.stdout.read(1)
        if not chunk:
            return None, line + process.stderr.read()
        line += chunk
    return line, b''


def serve_under(binary, data, hold):
    """Start mo serve while `hold` keeps the store; returns the listening delay."""
    holding = threading.Event()
    released = {}
    thread = threading.Thread(target=hold, args=(holding, released))
    thread.start()
    if not holding.wait(timeout=600):
        raise SystemExit('the reader never took the store')
    program = ROOT / 'services/task-queue/main.mo'
    started = time.monotonic()
    process = subprocess.Popen([str(binary), 'serve', str(program), '--port', '0', '--data',
                                str(data)], env={**os.environ, 'QUEUE_TOKEN': sizing.TOKEN},
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=ROOT)
    line, stderr = listening_after(process)
    listened = time.monotonic() - started
    thread.join(timeout=3600)
    if line is not None:
        process.send_signal(signal.SIGTERM)
    status = process.wait(timeout=120)
    process.stdout.close()
    process.stderr.close()
    return {'listened': line is not None, 'serve_seconds_to_listen': round(listened, 3),
            'reader_released_after_seconds': round(released.get('at', 0) - started, 3),
            'reader_status': released.get('status'),
            'serve_status': status, 'serve_stderr': stderr.decode('utf-8', 'replace')[-300:]}


def serve(binary, folder, hold_seconds):
    results = []
    with tempfile.TemporaryDirectory() as scratch:
        data = pathlib.Path(scratch) / 'data'
        shutil.copytree(folder, data)

        def python_reader(holding, released):
            connection = sqlite3.connect(f'file:{data / "service.sqlite"}?mode=ro', uri=True,
                                         isolation_level=None)
            connection.execute('BEGIN')
            connection.execute('SELECT count(*) FROM log').fetchone()
            holding.set()
            time.sleep(hold_seconds)
            connection.execute('COMMIT')
            connection.close()
            released['at'] = time.monotonic()

        results.append({'reader': f'python sqlite3, shared lock held {hold_seconds} s',
                        **serve_under(binary, data, python_reader)})

        def history_reader(holding, released):
            process = subprocess.Popen([str(binary), 'history', str(data)],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            holding.set()
            status = process.wait(timeout=3600)
            released['at'] = time.monotonic()
            released['status'] = status

        results.append({'reader': 'mo history started just before mo serve',
                        **serve_under(binary, data, history_reader)})
    for result in results:
        print(json.dumps({'folder': str(folder), **result}), flush=True)


def main(args):
    if len(args) < 3 or args[0] not in ('build', 'tail', 'reader', 'serve'):
        raise SystemExit(__doc__)
    binary = pathlib.Path(args[1]).resolve()
    if args[0] == 'build':
        build(binary, pathlib.Path(args[2]).resolve())
    elif args[0] == 'tail':
        tail(binary, pathlib.Path(args[2]).resolve())
    elif args[0] == 'reader':
        for folder in args[2:]:
            print(json.dumps(reader(binary, pathlib.Path(folder).resolve())), flush=True)
    else:
        serve(binary, pathlib.Path(args[2]).resolve(),
              float(args[3]) if len(args) > 3 else 3.0)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
