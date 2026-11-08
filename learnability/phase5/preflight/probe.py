#!/usr/bin/env python3
"""Bounded child-compiler and concrete old-type feasibility probes."""
import hashlib
import json
import pathlib
import resource
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[3]


def limits():
    resource.setrlimit(resource.RLIMIT_AS, (1 << 30, 1 << 30))
    resource.setrlimit(resource.RLIMIT_CPU, (10, 10))


def check(binary, source):
    started = time.monotonic()
    result = subprocess.run([str(binary), 'check', str(source)], capture_output=True,
                            timeout=15, preexec_fn=limits)
    return {'status': result.returncode, 'seconds': time.monotonic() - started,
            'stdout': result.stdout.decode(), 'stderr': result.stderr.decode()}


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    original = (ROOT / 'services/task-queue/job-queue.mo').read_text()
    declarations = original[original.index('struct Job\n'):original.index('enum QueueError\n')]
    with tempfile.TemporaryDirectory(prefix='mo-upgrade-preflight-') as directory:
        folder = pathlib.Path(directory)
        (folder / 'upgrade-old.mo').write_text(
            'module UpgradeOld\nexpose Queue(..), Job(..), JobState(..)\n' + declarations)
        source = '''module Main
use UpgradeOld{Queue}
struct NewQueue
  previous: Queue
  revision: Int
end
fn migrate(old: Queue) : NewQueue
  NewQueue(previous: old, revision: 1)
end
fn run() : Int
  migrate(UpgradeOld.Queue(jobs: {}, next_id: 1)).revision
end
'''
        path = folder / 'main.mo'
        path.write_text(source)
        good = check(binary, path)
        path.write_text(source.replace('migrate(UpgradeOld.Queue(jobs: {}, next_id: 1))', 'migrate(1)'))
        wrong = check(binary, path)
        path.write_text('module Main\nfn run() : Int\n' + '(' * 5000 + '1' + ')' * 5000 + '\nend\n')
        deep = check(binary, path)
        queue = check(binary, ROOT / 'services/task-queue/main.mo')
        started = time.monotonic()
        service_compile = subprocess.run([str(binary), 'check', '--emit=bytecode',
            str(ROOT / 'services/task-queue/main.mo')], capture_output=True,
            timeout=15, preexec_fn=limits)
        service_emit = {'status': service_compile.returncode,
                        'seconds': time.monotonic() - started,
                        'output_bytes': len(service_compile.stdout),
                        'stderr': service_compile.stderr.decode()}
        generic_source = 'module Main\nfn identity(value: T) : T\nvalue\nend\nfn main() : Int\n'
        for depth in range(1, 65):
            generic_source += f'_v{depth} = identity(' + '[' * depth + '1' + ']' * depth + ')\n'
        generic_source += '0\nend\n'
        path.write_text(generic_source)
        generics = check(binary, path)
        started = time.monotonic()
        emitted = subprocess.run([str(binary), 'emit', str(path), '-o', str(folder / 'generic.bc')],
                                 capture_output=True, timeout=15, preexec_fn=limits)
        generic_emit = {'status': emitted.returncode, 'seconds': time.monotonic() - started,
                        'stderr': emitted.stderr.decode()}

    output = {'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
              'old_declarations': good, 'wrong_argument': wrong,
              'deep_source': deep, 'task_queue': queue, 'generic_check': generics,
              'generic_emit': generic_emit, 'service_emit': service_emit}
    print(json.dumps(output, indent=2))
    assert good['status'] == 0 and queue['status'] == 0, output
    assert wrong['status'] == 1 and 'E0077' in wrong['stderr'], output
    assert deep['status'] == 1 and 'E1006' in deep['stderr'], output
    assert service_emit['status'] == 0 and service_emit['output_bytes'] > 0, output
    assert generics['status'] == 0 and generic_emit['status'] == 0, output


if __name__ == '__main__':
    main()
