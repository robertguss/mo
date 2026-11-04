#!/usr/bin/env python3
"""Can another compiled program decode a service's saved state? A prototype with today's decoder.

  python3 learnability/phase4/preflight/query_prototype.py MO

An offline state query is a different compiled program from the service that saved the state:
it adds a query function, in the state's defining module or in a module that imports it. Today
every saved record carries an image key derived from the whole program's checksum, so another
program is refused before its value is decoded. This prototype separates the two checks. It
fills a copy of the task queue past its first snapshot, then, for each variant program, copies
the data folder, rewrites every record's image key to the variant's own key (recomputing the
record checksum), and starts the variant on it with `mo serve`. With the image key out of the
way, only the saved schema comparison and handler replay decide whether the variant loads.

Variants:
  same             the unchanged program (the rewrite itself must not break loading)
  private-query    an added pure function inside JobQueue, the state's defining module
  imported-query   a new module Query that reads the state through JobQueue's exposed
                   accessor, used by a new read-only tool
  changed-schema   a field of the state's record type renamed: must be refused
  no-rewrite       private-query without the key rewrite: today's whole-program refusal

Prints one JSON object with each variant's outcome. The token is sizing.py's fixed canary.
"""

import json
import pathlib
import shutil
import sqlite3
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from sizing import Refused, Server  # noqa: E402

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent.parent
SOURCE = ROOT / 'services' / 'task-queue'
JOBS = 105
FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211
KEY_AT = 5 + 1 + 8  # magic and version, kind, sequence


def fnv(data):
    h = FNV_OFFSET
    for byte in data:
        h = ((h ^ byte) * FNV_PRIME) % (1 << 64)
    return h


def rekey(record, key):
    body = bytearray(record[:-8])
    body[KEY_AT:KEY_AT + 8] = struct.pack('<Q', key)
    return bytes(body) + struct.pack('<Q', fnv(body))


def check_record(record):
    assert record[:5] == b'MOST\x01', record[:5]
    assert struct.unpack('<Q', record[-8:])[0] == fnv(record[:-8]), 'record checksum'


def image_key(data):
    connection = sqlite3.connect(f'file:{data / "service.sqlite"}?mode=ro', uri=True)
    try:
        record = connection.execute('SELECT record FROM snapshot').fetchone()[0]
    finally:
        connection.close()
    check_record(record)
    return struct.unpack('<Q', record[KEY_AT:KEY_AT + 8])[0]


def rewrite(data, key):
    connection = sqlite3.connect(str(data / 'service.sqlite'))
    try:
        rows = [('snapshot', 'id', row_id, record) for row_id, record in
                connection.execute('SELECT id, record FROM snapshot')]
        rows += [('log', 'seq', row_id, record) for row_id, record in
                 connection.execute('SELECT seq, record FROM log')]
        for table, column, row_id, record in rows:
            check_record(record)
            connection.execute(f'UPDATE {table} SET record = ? WHERE {column} = ?',
                               (rekey(record, key), row_id))
        connection.commit()
    finally:
        connection.close()
    return len(rows)


def value(server, tool, **arguments):
    """A successful tool call's value."""
    content, failed, _, _ = server.call(tool, **arguments)
    assert not failed, (tool, content)
    return content['value']


def stopped(server):
    status, _, _ = server.stop()
    return status


def edit(folder, name, old, new):
    path = folder / name
    text = path.read_text(encoding='utf-8')
    assert text.count(old) == 1, (name, old)
    path.write_text(text.replace(old, new), encoding='utf-8')


QUERY_MODULE = '''## A query over the queue's state, written outside the module that defines it.
module Query
expose ready
use JobQueue{Queue, JobState}

## How many jobs are ready to lease.
fn ready(queue: Queue) : Int
  var count = 0
  for job in JobQueue.jobs(queue)
    if job.state == Ready
      count = count + 1
    end
  end
  count
end
'''


def same_counts(a, b):
    """Equal stats apart from the reading's own time."""
    return {k: v for k, v in a.items() if k != 'now'} == {k: v for k, v in b.items() if k != 'now'}


def make_variant(base, name):
    folder = base / name
    shutil.copytree(SOURCE, folder)
    if name in ('private-query', 'no-rewrite'):
        edit(folder, 'job-queue.mo', '\njobs\n', '\njobs,\nnext_id_of\n')
        with open(folder / 'job-queue.mo', 'a', encoding='utf-8') as f:
            f.write('\n## The id the next submitted job will get.\n'
                    'fn next_id_of(queue: Queue) : Int\n  queue.next_id\nend\n')
    elif name == 'imported-query':
        (folder / 'query.mo').write_text(QUERY_MODULE, encoding='utf-8')
        edit(folder, 'main.mo', 'use JobQueue{Queue, Job, QueueError}\n',
             'use JobQueue{Queue, Job, QueueError}\nuse Query\n')
        edit(folder, 'main.mo', 'list, stats\n', 'list, stats, ready\n')
        with open(folder / 'main.mo', 'a', encoding='utf-8') as f:
            f.write('\n## How many jobs are ready to lease.\n'
                    'fn ready(queue: Queue, _now: Time) : Int\n  Query.ready(queue)\nend\n')
    elif name == 'changed-schema':
        path = folder / 'job-queue.mo'
        text = path.read_text(encoding='utf-8')
        assert text.count('next_id') > 1
        path.write_text(text.replace('next_id', 'next_number'), encoding='utf-8')
    return folder


def main(args):
    binary = pathlib.Path(args[0]).resolve()
    results = {}
    with tempfile.TemporaryDirectory() as temporary:
        base = pathlib.Path(temporary)
        original = base / 'original'
        shutil.copytree(SOURCE, original)
        saved = base / 'saved'
        saved.mkdir()
        server = Server(binary, original / 'main.mo', 'QUEUE_TOKEN', saved)
        for n in range(JOBS):
            value(server, 'submit', payload=f'job {n}')
        value(server, 'lease')
        before = value(server, 'stats')
        assert stopped(server) == 0
        results['original'] = {'stats': before, 'image_key': f'{image_key(saved):016x}'}
        for name in ('same', 'private-query', 'imported-query', 'changed-schema', 'no-rewrite'):
            folder = make_variant(base, name) if name != 'same' else original
            check = subprocess.run([str(binary), 'check', str(folder / 'main.mo')],
                                   capture_output=True, text=True, timeout=120)
            if check.returncode != 0:
                results[name] = {'check': check.stdout + check.stderr}
                continue
            fresh = base / f'{name}-fresh'
            fresh.mkdir()
            assert stopped(Server(binary, folder / 'main.mo', 'QUEUE_TOKEN', fresh)) == 0
            key = image_key(fresh)
            data = base / f'{name}-data'
            shutil.copytree(saved, data)
            rewritten = 0 if name == 'no-rewrite' else rewrite(data, key)
            outcome = {'image_key': f'{key:016x}', 'records_rewritten': rewritten}
            try:
                variant = Server(binary, folder / 'main.mo', 'QUEUE_TOKEN', data)
            except Refused as refusal:
                outcome.update(loaded=False, exit_status=refusal.status, refusal=refusal.stderr)
            else:
                outcome['loaded'] = True
                outcome['stats'] = value(variant, 'stats')
                outcome['stats_match'] = same_counts(outcome['stats'], before)
                if name == 'imported-query':
                    outcome['ready'] = value(variant, 'ready')
                outcome['exit_status'] = stopped(variant)
            results[name] = outcome
    print(json.dumps(results, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
