#!/usr/bin/env python3
"""Explicit DEVELOPMENT debugger controls; never a timing or final drill run."""
import argparse
import hashlib
import json
import pathlib
import subprocess
import tempfile
import threading
import time

from check_kill_loop import Run, ALL_JOBS


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mo')
    args = parser.parse_args()
    binary = pathlib.Path(args.mo).resolve()
    root = pathlib.Path(tempfile.mkdtemp(prefix='mo-kill-stages-'))
    print(f'DEVELOPMENT kills=6 seed=20261110 artifacts={root}', flush=True)
    run = Run(binary, 20261110, 6, root)
    # Unique executable source anchors: fail closed when source changes.
    sites = [
        ('upload', 'src/tools/protocol-upgrade.c', 'enum mo_upload_status status = mo_upload_page('),
        ('compiled_staged', 'src/tools/upgrade-live.c', 'u->staged = mo_upgrade_compile_end('),
        ('ordinary_call', 'src/vm/execute.c', 'uint32_t head = fn->code[f->pc], op = head & MO_OPCODE_MASK;'),
        ('migration', 'src/tools/upgrade.c',
         'enum mo_outcome result = mo_call(process, selected, &argument, 1, &value, &defect);'),
        ('transaction_interior', 'src/host/store-programs.c',
         'bool base_read = store_current_head(s, &head, &empty) == MO_STORE_OK && !empty &&'),
        ('postcommit_before_reply', 'src/tools/protocol-upgrade.c',
         'PAUSE_START(committed_at);'),
    ]
    debugger = None
    pusher = None
    try:
        run.variants()
        run.digests = {i: run.digest(i) for i in (0, 1, 2, 4)}
        for stage, source, anchor in sites:
            run.new_folder()
            for call in [('submit', {'payload': run.payload()}), ('lease', {})]:
                assert run.judge(call, run.send(call)) is None
            before = run.listing(ALL_JOBS['case'])
            lines = pathlib.Path(source).read_text().splitlines()
            matches = [i + 1 for i, line in enumerate(lines) if anchor in line]
            assert len(matches) == 1, (source, matches)
            line = matches[0]
            marker = root / (stage + '.hit')
            ready = root / (stage + '.ready')
            script = root / (stage + '.gdb')
            extra = ''
            if stage == 'migration':
                vm_lines = pathlib.Path('src/vm/execute.c').read_text().splitlines()
                vm_line = next(i + 1 for i, text in enumerate(vm_lines)
                               if 'uint32_t head = fn->code[f->pc]' in text)
                extra = (f'disable 1\nbreak src/vm/execute.c:{vm_line}\n'
                         'ignore 2 1\ncontinue\n'
                         'python f=gdb.newest_frame(); f=f.older(); f=f.older(); assert f.name()=="mo_call" and int(f.read_var("argument_count"))==1\n')
            elif stage == 'compiled_staged':
                extra = 'next\npython assert int(gdb.parse_and_eval("u->staged")) == 1\n'
            script.write_text(
                'set pagination off\nset confirm off\n'
                f'attach {run.queue.process.pid}\n'
                f'break {source}:{line}\n'
                f'python open({str(ready)!r}, "w").write("ready")\ncontinue\n'
                + extra + 'bt\n'
                f'python open({str(marker)!r}, "w").write("observed")\nkill\nquit\n')
            log = (root / (stage + '.log')).open('w')
            debugger = subprocess.Popen(['/bin/gdb', '-q', '-batch', '-x', str(script),
                                         str(binary)], stdout=log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 20
            while not ready.exists() and debugger.poll() is None and time.monotonic() < deadline:
                time.sleep(.05)
            assert ready.exists(), f'gdb attach failed: {root / (stage + ".log")}'
            out = {}
            pusher = (threading.Thread(target=run.send, args=(('stats', {}),))
                      if stage == 'ordinary_call' else
                      threading.Thread(target=run.push, args=(4, out)))
            pusher.start()
            debugger.wait(timeout=60)
            log.close()
            assert marker.exists(), f'stage not observed: {stage}'
            pusher.join(timeout=310)
            assert not pusher.is_alive()
            run.kill()
            if stage == 'postcommit_before_reply':
                run.program, run.ordinal = 4, 1
            run.restart(('stats', {}), [run.model])
            assert run.listing(ALL_JOBS['case']) == before
            if run.program == 0:
                assert run.recorded == 0
                run.upgrade(4, False, load=False)
            else:
                retry = {}
                run.push(4, retry)
                assert retry['result'].returncode == 0, retry
            run.queue.stop()
            run.queue = None
            run.history()
            assert run.recorded == 1
            print(json.dumps({'stage': stage, 'source': source, 'line': line,
                              'source_sha256': hashlib.sha256(pathlib.Path(source).read_bytes()).hexdigest(),
                              'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
                              'head': subprocess.check_output(['git', 'rev-parse', 'HEAD']).decode().strip(),
                              'result': 'pass'}), flush=True)
    finally:
        if debugger is not None and debugger.poll() is None:
            debugger.kill()
            debugger.wait()
        if run.queue is not None:
            run.queue.kill()
        if pusher is not None:
            pusher.join(timeout=310)


if __name__ == '__main__':
    main()
