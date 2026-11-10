"""Bounded opt-in measurement of one existing DEVELOPMENT bundle; no collector."""
import os
import sys
import json
import time
import resource
from pathlib import Path
import threading


def memory(pid='self'):
    try:
        return {k: int(v.split()[0]) for k, v in
                (line.split(':', 1) for line in Path(f'/proc/{pid}/status').read_text().splitlines())
                if k in ('VmRSS', 'VmHWM')}
    except (OSError, ProcessLookupError):
        return {}


def main():
    os.umask(0o077)
    mode, destination = sys.argv[1:]
    start = time.monotonic()
    initial = dict(memory(), ru_maxrss=resource.getrusage(0).ru_maxrss)
    peaks = {'self': 0, 'children_sum': 0, 'child_individual': 0}
    stop = threading.Event()
    def sample():
        while not stop.is_set():
            peaks['self'] = max(peaks['self'], memory().get('VmRSS', 0))
            pending = [os.getpid()]
            rss = []
            while pending:
                pid = pending.pop()
                try:
                    children = Path(f'/proc/{pid}/task/{pid}/children').read_text().split()
                except OSError:
                    children = []
                for child in children:
                    pending.append(int(child))
                    rss.append(memory(child).get('VmRSS', 0))
            peaks['children_sum'] = max(peaks['children_sum'], sum(rss))
            peaks['child_individual'] = max([peaks['child_individual'], *rss])
            stop.wait(.01)
    thread = threading.Thread(target=sample)
    thread.start()
    result = {}
    try:
        if mode in ('stream', 'regrade'):
            import pwd
            import hashlib
            import protected_regrade as replay
            from test_real_replay_development import pins
            root = Path('/home/exedev/phase5-test-private-6uner697')
            session = root / 'run-success'
            bundle = session / 'replay-private'
            before = pins(bundle)
            assert before == json.loads((root / 'control-original-pins.json').read_text())
            uid = pwd.getpwnam('mo-phase5-test').pw_uid
            manifest = replay.replay_bundle.verify(bundle, str(session), uid)
            if mode == 'regrade':
                verdict = replay.regrade(bundle, session_id=str(session), session_uid=uid,
                                         workspace_parent=Path(destination).parent)
                result.update(dimensions=verdict['dimensions'], correct=verdict['correct'],
                              evidence_class=verdict['evidence_class'])
                assert not verdict['correct'] and verdict['evidence_class'] == 'DEVELOPMENT'
            else:
                import origin_trace
                from trace_storage import TraceLines, decoded
                snapshot = json.loads((bundle / 'tcp_snapshot').read_text())
                parsed = origin_trace.parse_trace(TraceLines(bundle / 'trace'), session_id=str(session),
                    tcp_snapshots=snapshot['snapshots'], evaluation_endpoint=manifest['provenance']['evaluation_endpoint'])
                integrity = replay.origin_observer.process_integrity(TraceLines(bundle / 'trace'))
                result.update(requests=len(parsed['requests']), responses=len(parsed['responses']),
                              process_completeness_verified=integrity['process_completeness_verified'])
                digest = hashlib.sha256(); count = 0
                with decoded(bundle / 'trace') as source:
                    while chunk := source.read(65536):
                        digest.update(chunk); count += len(chunk)
                result.update(decoded_bytes=count, decoded_sha256=digest.hexdigest())
            assert pins(bundle) == before
            replay.replay_bundle.verify(bundle, str(session), uid)
            result.update(pins_unchanged=True, stored_trace_bytes=(bundle/'trace').stat().st_size,
                          bundle_bytes=sum(p.stat().st_size for p in bundle.iterdir()))
        elif mode == 'probe':
            block = bytearray(64 * 1024 * 1024)
            result['allocated'] = dict(memory(), ru_maxrss=resource.getrusage(0).ru_maxrss)
            pid = os.fork()
            if pid == 0:
                os.execv(sys.executable, [sys.executable, __file__, 'baseline', destination + '.child.json'])
            _, status = os.waitpid(pid, 0)
            assert status == 0
            del block
        else:
            assert mode == 'baseline'
    finally:
        stop.set(); thread.join()
        result.update(mode=mode, initial=initial, final=dict(memory(), ru_maxrss=resource.getrusage(0).ru_maxrss),
                      sampled_peak_kib=peaks, elapsed_seconds=time.monotonic()-start, sample_interval_seconds=.01)
        Path(destination).write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result), flush=True)

if __name__ == '__main__':
    main()
