#!/usr/bin/env python3
"""What a stopped task queue's data folder still says about a job, once a snapshot has passed.

  python3 learnability/phase4/preflight/history_trace.py MO [FILLER]

Submits one job, leases and fails it until it is dead (three attempts), then sends FILLER more
updating calls (default 120, past the snapshot taken every 100 messages) and stops the queue.
It then reads the data folder directly: the snapshot's sequence, the log tail's sequences, and
which tool each tail record names. It prints those beside what the live `list` tool said about
the job before stopping, so the record shows which history survives a snapshot (the current
state, with the job's attempt count) and which does not (the lease and fail calls that killed
it, and their arrival times). The token is a fixed canary.
"""

import json
import pathlib
import sqlite3
import struct
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from sizing import Server  # noqa: E402

TOOLS = ['submit', 'lease', 'heartbeat', 'acknowledge', 'fail', 'list', 'stats']
PAYLOAD_AT = 5 + 1 + 8 + 8 + 4  # magic and version, kind, sequence, image key, length


def tail(data):
    connection = sqlite3.connect(f'file:{data / "service.sqlite"}?mode=ro', uri=True)
    try:
        snapshot = connection.execute('SELECT record FROM snapshot').fetchone()[0]
        rows = connection.execute('SELECT seq, record FROM log ORDER BY seq').fetchall()
    finally:
        connection.close()
    snapshot_seq = struct.unpack('<Q', snapshot[6:14])[0]
    records = []
    for seq, record in rows:
        timer = record[PAYLOAD_AT]
        index = struct.unpack('<I', record[PAYLOAD_AT + 1:PAYLOAD_AT + 5])[0]
        records.append((seq, 'expire' if timer else TOOLS[index]))
    return snapshot_seq, records


def main(args):
    binary = pathlib.Path(args[0]).resolve()
    filler = int(args[1]) if len(args) > 1 else 120
    with tempfile.TemporaryDirectory() as folder:
        data = pathlib.Path(folder)
        server = Server(binary, 'services/task-queue/main.mo', 'QUEUE_TOKEN', data)
        job, _, _, _ = server.call('submit', payload='the job that dies')
        job_id = job['value']
        killing = []
        for attempt in range(1, 4):
            leased, _, _, _ = server.call('lease')
            killing.append(('lease', leased['value']['case']))
            failed, is_error, _, _ = server.call('fail', id=job_id, attempt=str(attempt))
            killing.append(('fail', 'error' if is_error else 'ok'))
        for n in range(filler):
            server.call('submit', payload=f'filler {n}')
        listed, _, _, _ = server.call('list', filter={'case': 'AllJobs'}, after='0', limit='1')
        status, stderr, _ = server.stop()
        snapshot_seq, records = tail(data)
    tools_in_tail = sorted({tool for _, tool in records})
    result = {
        'killing_calls': killing,
        'updating_calls_total': 1 + len(killing) + filler,
        'live_view_of_job': listed['value']['jobs'][0],
        'snapshot_sequence': snapshot_seq,
        'log_tail_sequences': [records[0][0], records[-1][0]] if records else [],
        'tools_in_log_tail': tools_in_tail,
        'lease_or_fail_in_tail': any(t in ('lease', 'fail') for t in tools_in_tail),
        'exit_status': status,
        'stderr': stderr.strip(),
    }
    print(json.dumps(result, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
