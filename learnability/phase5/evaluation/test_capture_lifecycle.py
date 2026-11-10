"""Bounded failure controls for collector lifecycle; no model sessions."""
import pathlib
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
import origin_observer


class TracerFailures(unittest.TestCase):
    def test_normal_root_exit_cleans_descendant_without_timeout(self, force_wait=False):
        import os
        import signal
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            pidfile = root / 'descendant'
            script = ('import os,time,pathlib; pid=os.fork(); '
                      'pathlib.Path(' + repr(str(pidfile)) + ').write_text(str(pid)) if pid else time.sleep(30)')
            def cleanup():
                os.kill(int(pidfile.read_text()), signal.SIGTERM)
            run = origin_observer.TraceRun(root / 'capture')
            start = time.monotonic()
            real = subprocess.Popen
            def launch(*args, **kwargs):
                child = real(*args, **kwargs)
                if args[0][0] == 'strace' and force_wait:
                    original = child.wait
                    fired = False
                    def wait(timeout=None):
                        nonlocal fired
                        if timeout == 5 and not fired:
                            fired = True
                            raise subprocess.TimeoutExpired('injected drain', timeout)
                        return original(timeout=timeout)
                    child.wait = wait
                return child
            with patch.object(origin_observer.subprocess, 'Popen', launch):
                result = run.run([sys.executable, '-c', script], timeout=15, cleanup=cleanup)
            self.assertLess(time.monotonic()-start, 10)
            self.assertFalse(result['timed_out'])
            self.assertEqual(result['returncode'], 0)
            if not force_wait:
                self.assertTrue(result['process_completeness_verified'], result)
            import gzip
            raw = gzip.decompress((run.directory/'trace').read_bytes())
            self.assertEqual(len(raw), result['decoded_trace']['bytes'])

    def test_root_drain_timeout_still_finishes_compression(self):
        self.test_normal_root_exit_cleans_descendant_without_timeout(force_wait=True)

    def test_compressor_exits_before_fifo_open(self):
        real = subprocess.Popen
        launched = []
        def launch(args, **kwargs):
            if args[0] == sys.executable:
                args = [sys.executable, '-c', 'raise SystemExit(7)']
            child = real(args, **kwargs)
            launched.append(child)
            return child
        with tempfile.TemporaryDirectory() as tmp, patch.object(origin_observer.subprocess, 'Popen', launch):
            run = origin_observer.TraceRun(pathlib.Path(tmp) / 'capture')
            start = time.monotonic()
            with self.assertRaisesRegex(RuntimeError, 'compressor|compression'):
                run.run(['/bin/true'], timeout=30)
            self.assertLess(time.monotonic() - start, 12)
            self.assertTrue(all(p.poll() is not None for p in launched))
            self.assertFalse((run.directory / 'trace.fifo').exists())

    def test_timeout_cleanup_exception_still_reaps_tracer(self):
        real = subprocess.Popen
        launched = []
        def launch(*args, **kwargs):
            child = real(*args, **kwargs)
            launched.append(child)
            return child
        def fail_cleanup():
            raise RuntimeError('cleanup injected')
        with tempfile.TemporaryDirectory() as tmp, patch.object(origin_observer.subprocess, 'Popen', launch):
            run = origin_observer.TraceRun(pathlib.Path(tmp) / 'capture')
            with self.assertRaisesRegex(RuntimeError, 'cleanup injected'):
                run.run(['/bin/sleep', '30'], timeout=.1, cleanup=fail_cleanup)
            self.assertTrue(all(p.poll() is not None for p in launched))
            self.assertTrue(run.timed_out)
            self.assertFalse((run.directory / 'trace.fifo').exists())


class CollectorFailures(unittest.TestCase):
    def exercise(self, *, stop_failure=False, source_failure=False, verification_failure=False, bundle_failure=None, state_failure=False):
        import contextlib
        import io
        import json
        import shutil
        from types import SimpleNamespace
        import harness as h
        with tempfile.TemporaryDirectory() as tmp, contextlib.ExitStack() as stack:
            root = pathlib.Path(tmp)
            state = root / 'state'; state.mkdir()
            source = root / 'source'; source.mkdir(); (source / 'main.mo').write_text('original')
            data = root / 'data'; data.mkdir()
            expected = root / 'expected'; expected.write_text('{"revision":"1"}')
            prompt = root / 'prompt'; prompt.write_text('SERVICE_URL')
            spec = root / 'spec'; spec.write_text(json.dumps(dict(source=str(source), context=str(source),
                data=str(data), expected=str(expected), prompt=str(prompt), experiment=str(root/'experiment'))))
            out = root / 'out'
            l = SimpleNamespace(work=root/'work', state=state, tokens=state/'tokens', secrets=state/'secrets',
                deployed=root/'deployed', service_log=root/'service-log', mcp_config=root/'mcp',
                bin=root/'bin', user='fake-session', account=lambda: SimpleNamespace(pw_uid=65534),
                home=lambda: root/'home')
            l.bin.mkdir(); (l.bin/'mo').symlink_to('/bin/true')
            class Process:
                stdout = io.BytesIO()
                stopped = False
                def poll(self): return 0 if self.stopped else None
                def terminate(self):
                    if stop_failure: raise OSError('injected service termination')
                    self.stopped = True
                def wait(self, timeout=None): return 0
            process = Process()
            class Service:
                def listening(self): return 12345
            class Capture:
                def __init__(self, directory, session, uid, binary):
                    directory.mkdir(); self.session=session; self.session_uid=uid; self.nonce='nonce'
                def seal(self, *args, **kwargs): pass
            class Trace:
                def __init__(self, directory): self.directory=directory; directory.mkdir()
                def run(self, *args, **kwargs):
                    for name in ('stdout','stderr','trace'): (self.directory/name).write_text('')
                    return dict(returncode=0, timed_out=False, capture_finished=True)
            class Recorder:
                url='http://127.0.0.1:12346/mcp'
                recorder=SimpleNamespace(snapshots=[], clean=lambda x:x)
                def __init__(self, url, path, *args): path.write_text('')
                def stop(self): pass
            partials=[]
            class Bundle:
                def __init__(self, path, *args, **kwargs):
                    self.root=path; path.mkdir(); self.facts={}; self.files={}
                    if bundle_failure=='constructor': raise ValueError('injected constructor')
                def add(self, *args, **kwargs):
                    if bundle_failure=='population': raise ValueError('injected file limit')
                def tree(self, *args): pass
                def finish(self, *args):
                    if bundle_failure=='finish': raise ValueError('injected finish')
                    (self.root/'manifest.json').write_text('{}')
            class Partial:
                def __init__(self, path, *args):
                    self.root=path; path.mkdir(); self.facts={'available':[]}; self.files={}; partials.append(self)
                def add(self, *args, **kwargs): pass
                def tree(self, role, path):
                    if role == 'state' and state_failure: raise OSError('retention injected')
                    self.facts[role]=True
                def finish(self, facts):
                    self.launch=facts; (self.root/'manifest.json').write_text('{}')
            def collect_work(l, out):
                if source_failure: raise OSError('injected capture failure')
                shutil.copytree(l.work, out/l.work.name); shutil.rmtree(l.work); return []
            def mocked(owner, name, **kwargs): return stack.enter_context(patch.object(owner,name,**kwargs))
            mocked(h.p4,'check_private'); reset=mocked(h.p4,'reset')
            mocked(h.p4,'append_secret'); mocked(h.p4,'place_read_only'); mocked(h.p4,'end_all')
            mocked(h.p4,'as_session_user',return_value=(0,''))
            mocked(h.p4,'sudo',return_value=SimpleNamespace(stdout=b'baseline'))
            mocked(h.p4,'collect_work',side_effect=collect_work); mocked(h.p4,'artifacts',return_value={})
            mocked(h.p4.fixture,'Service',new=Service)
            mocked(h.subprocess,'Popen',return_value=process)
            mocked(h.subprocess,'run',return_value=SimpleNamespace(returncode=0 if verification_failure else 1,stdout='42' if verification_failure else ''))
            mocked(h.grader.fixture,'Client'); mocked(h.grader,'precheck',return_value=True)
            mocked(h.grader,'live_grade',return_value={})
            grade=mocked(h,'grade_collected')
            mocked(h.origin_identity,'Capture',new=Capture)
            mocked(h.origin_identity,'protected',return_value=({'sha256':'fake'},None))
            mocked(h.origin_observer,'TraceRun',new=Trace)
            mocked(h.proxy,'Proxy',new=Recorder); mocked(h.proxy,'match',return_value={'match':False})
            mocked(h.replay_bundle,'Bundle',new=Bundle); mocked(h.replay_bundle,'PartialBundle',new=Partial)
            self.assertEqual(h.collect(l,spec,out,1,development=True,limit=1),1)
            meta=json.loads((out/'manifest.json').read_text())
            self.assertFalse(meta['correct'])
            if stop_failure:
                self.assertFalse(meta['service_stopped']); grade.assert_not_called()
                self.assertNotIn('state',partials[0].facts)
            elif source_failure or verification_failure:
                self.assertEqual((l.work/'main.mo').read_text(),'original')
                self.assertEqual(reset.call_count,1)
                self.assertTrue((state/'retention-hold.json').exists())
                with self.assertRaisesRegex(ValueError,'uncaptured work'): h.require_no_retention_hold(l)
                grade.assert_not_called()
            else:
                self.assertEqual(len(partials),1)
                self.assertEqual(partials[0].root,out/'replay-partial-private')
                self.assertTrue((partials[0].root/'manifest.json').exists())
                self.assertEqual(meta['replay_path'],str(partials[0].root))
                if state_failure: grade.assert_not_called()
                else: grade.assert_called_once()

    def test_partial_state_failure_blocks_mutating_grade(self):
        self.exercise(bundle_failure='constructor', state_failure=True)

    def test_live_service_blocks_sealing_and_grading(self): self.exercise(stop_failure=True)
    def test_source_capture_failure_preserves_work_and_blocks_reset(self): self.exercise(source_failure=True)
    def test_unverified_process_cleanup_preserves_work(self): self.exercise(verification_failure=True)
    def test_full_bundle_failure_falls_back_at_each_stage(self):
        for stage in ('constructor','population','finish'):
            with self.subTest(stage=stage): self.exercise(bundle_failure=stage)


class RetentionRollback(unittest.TestCase):
    def test_tree_limit_keeps_receipt_valid_and_originals_intact(self):
        import os
        import replay_bundle as r
        with tempfile.TemporaryDirectory(dir=pathlib.Path.home()) as tmp:
            root=pathlib.Path(tmp)
            source=root/'source'; source.mkdir()
            for name in ('a','b','c'): (source/name).write_text(name)
            bundle=r.PartialBundle(root/'retained','session',os.getuid()+1)
            bundle.add('expected',source/'a')
            total=bundle.total
            with patch.object(r,'MAX_FILES',2), self.assertRaises(ValueError):
                bundle.tree('candidate',source)
            self.assertEqual(set(bundle.files),{'expected'})
            self.assertEqual(bundle.total,total)
            self.assertEqual({p.name for p in bundle.root.iterdir()},{'expected'})
            self.assertEqual([p.read_text() for p in sorted(source.iterdir())],['a','b','c'])
            bundle.finish(dict(launched=True,development=True,slot=1,client='codex',timeout=False,
                              cleanup_zero_processes=True,firewall_restored=True))
            self.assertEqual(r.verify(bundle.root,'session',os.getuid()+1)['provenance']['available'],[])


class PartialCallback(unittest.TestCase):
    def test_callback_uses_protected_partial_after_failed_full_attempt(self):
        import protected_regrade as r
        with tempfile.TemporaryDirectory() as tmp:
            root=pathlib.Path(tmp); (root/'replay-private').mkdir()
            (root/'replay-partial-private').mkdir()
            callback=r.callback({root.resolve():('session',123)},root.parent)
            with patch.object(r,'regrade',return_value={'correct':False}) as regrade:
                self.assertFalse(callback(root)['correct'])
                self.assertEqual(regrade.call_args.args[0],root/'replay-partial-private')


class RetentionExhaustion(unittest.TestCase):
    def test_work_inspection_errors_and_dangling_links_refuse_reset(self):
        from types import SimpleNamespace
        import harness as h
        with tempfile.TemporaryDirectory() as tmp:
            root=pathlib.Path(tmp); l=SimpleNamespace(work=root/'work',state=root)
            with patch.object(pathlib.Path,'lstat',side_effect=PermissionError('unreadable')):
                with self.assertRaises(PermissionError): h.require_no_retention_hold(l)
            l.work.symlink_to(root/'missing')
            with self.assertRaisesRegex(ValueError,'uncaptured work'): h.require_no_retention_hold(l)

    def test_failed_hold_write_still_blocks_setup_and_collection_reset(self):
        from types import SimpleNamespace
        import harness as h
        with tempfile.TemporaryDirectory() as tmp:
            root=pathlib.Path(tmp)
            work=root/'work'; work.mkdir(); (work/'result.mo').write_text('uncaptured work')
            l=SimpleNamespace(work=work,state=root,bin=root/'bin')
            with patch.object(h.origin_identity,'atomic_private',side_effect=OSError('disk full')):
                with self.assertRaises(OSError):
                    h.reset_preserving_work(l,root,dict(launched=True,source_captured=False))
            self.assertFalse((root/'retention-hold.json').exists())
            with patch.object(h.p4,'setup') as setup, patch.object(h.p4,'sudo'):
                with self.assertRaisesRegex(ValueError,'uncaptured work'):
                    h.setup(l,'mo','claude','codex')
                setup.assert_not_called()
            with self.assertRaisesRegex(ValueError,'uncaptured work'):
                h.require_no_retention_hold(l)
            self.assertEqual((work/'result.mo').read_text(),'uncaptured work')

    def test_optional_budget_failure_finishes_verifiable_replay(self):
        import os
        import harness as h
        import replay_bundle as r
        import protected_regrade as regrade
        for limit_name,limit in (('MAX_BYTES',3),('MAX_FILES',1)):
            with self.subTest(limit=limit_name), tempfile.TemporaryDirectory(dir=pathlib.Path.home()) as tmp:
                root=pathlib.Path(tmp); source=root/'source'; source.mkdir()
                (source/'small').write_bytes(b'x')
                trace=root/'trace'; trace.write_bytes(b'oversized')
                bundle=r.PartialBundle(root/'replay-partial-private',str(root),os.getuid()+1)
                bundle.tree('original',source); bundle.facts['available'].append('original')
                with patch.object(r,limit_name,limit):
                    h.retain_optional(bundle,'trace',trace)
                self.assertNotIn('trace',bundle.files)
                self.assertIn('trace',bundle.facts['unavailable'])
                bundle.finish(dict(launched=True,development=True,slot=1,client='codex',timeout=False,
                    cleanup_zero_processes=True,firewall_restored=True))
                r.verify(bundle.root,str(root),os.getuid()+1)
                verdict=regrade.callback({root.resolve():(str(root),os.getuid()+1)},root)(root)
                self.assertFalse(verdict['correct'])
                self.assertTrue(all(v is False for v in verdict['dimensions'].values()))
