"""Development tests; no service drill or agent session is collected here."""
import unittest
from evidence import evaluation_gate, build_inputs


class EvidenceTests(unittest.TestCase):
    def test_missing_and_duplicate_slots_cannot_pass(self):
        rows = [{'slot': n, 'client': c, 'correct': True}
                for n, c in enumerate(['codex', 'claude', 'codex', 'claude'], 1)]
        self.assertTrue(evaluation_gate(rows, True)['passed'])
        self.assertFalse(evaluation_gate(rows[:-1], True)['passed'])
        self.assertFalse(evaluation_gate(rows + rows[:1], True)['passed'])
        self.assertFalse(evaluation_gate(rows, False)['passed'])

    def test_one_miss_permitted_but_not_a_missing_client(self):
        rows = [{'slot': n, 'client': c, 'correct': n != 1}
                for n, c in enumerate(['codex', 'claude', 'codex', 'claude'], 1)]
        self.assertTrue(evaluation_gate(rows, True)['passed'])
        rows[2]['correct'] = False
        self.assertFalse(evaluation_gate(rows, True)['passed'])

    def test_development_never_counts(self):
        rows = [{'slot': n, 'client': c, 'correct': True, 'development': True}
                for n, c in enumerate(['codex', 'claude', 'codex', 'claude'], 1)]
        self.assertFalse(evaluation_gate(rows, True)['passed'])


class FreezeTests(unittest.TestCase):
    def test_collector_accepts_only_the_unchanged_freeze(self):
        import json
        import subprocess
        import sys
        import tempfile
        from pathlib import Path
        from types import SimpleNamespace
        import evidence
        sys.path.insert(0, str(Path(__file__).resolve().with_name('evaluation')))
        import harness
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp).resolve()
            repo = tmp / 'repo'
            subprocess.run(['git', 'clone', '--quiet', '--shared', str(evidence.ROOT), str(repo)], check=True)
            subprocess.run(['git', '-C', str(repo), 'update-ref', 'refs/remotes/origin/main', 'HEAD'], check=True)
            head = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip()
            folder = tmp / 'freeze'
            inputs = {}
            for name in ('source', 'data'):
                inputs[name] = tmp / name
                inputs[name].mkdir()
                (inputs[name] / 'main.mo').write_text(name)
            for name in ('prompt', 'expected'):
                inputs[name] = tmp / name
                inputs[name].write_text(name)
            folder.mkdir()
            (folder / 'spec.json').write_text(json.dumps(dict({k: str(v) for k, v in inputs.items()},
                                                              context=str(inputs['source']))))
            (folder / 'verification.json').write_text(json.dumps(dict(commit=head, independently_checked=True,
                                                                      passed=True)))
            controls = dict.fromkeys(('real_service_grading', 'proxy_commit', 'credential_redaction',
                                      'collector_success', 'collector_timeout', 'source_tests',
                                      'native_upgrade_header'), True)
            for name in ('build-commands.json', 'compiler-versions.json'):
                (folder / name).write_text('{}')
            installed = tmp / 'bin'
            installed.mkdir()
            for name in evidence.CLIENTS:
                (installed / name).write_text(name)
            debug, optimized = tmp / 'debug-mo', tmp / 'optimized-mo'
            debug.write_text('debug')
            optimized.write_text('optimized')

            def frozen(controls):
                (folder / 'controls.json').write_text(json.dumps(controls))
                evidence.freeze(repo, folder, 'experiment-3', installed, debug, optimized)
                return harness.freeze(folder / 'manifest.json', SimpleNamespace(bin=installed))

            manifest = frozen(controls)
            self.assertEqual(manifest['candidate_commit'], head)
            for name in evidence.INSTRUMENTS:
                self.assertIn(str(repo / name), manifest['artifacts']['instruments'])
            for binary in (debug, optimized, *(installed / name for name in evidence.CLIENTS)):
                self.assertIn(str(binary), manifest['artifacts']['binaries'])
            self.assertIn(str(repo / 'learnability/phase5/evaluation/harness.py'), manifest['artifacts']['harness'])
            missing = dict(controls)
            del missing['proxy_commit']
            with self.assertRaisesRegex(ValueError, 'required development controls'):
                frozen(missing)
            frozen(controls)
            debug.write_text('rebuilt')
            with self.assertRaisesRegex(ValueError, 'frozen artifact changed'):
                harness.freeze(folder / 'manifest.json', SimpleNamespace(bin=installed))
            debug.write_text('debug')
            with (repo / 'learnability/phase5/evaluation/harness.py').open('a') as changed:
                changed.write('\n')
            with self.assertRaises(ValueError):
                harness.freeze(folder / 'manifest.json', SimpleNamespace(bin=installed))


if __name__ == '__main__':
    unittest.main()
