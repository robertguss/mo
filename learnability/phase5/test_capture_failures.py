"""Evidence survives collector timeout and malformed compiler output."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import compiler_load
import measure_pause


class Retention(unittest.TestCase):
    def test_partial_timeout_output(self):
        for module in (compiler_load, measure_pause):
            with self.subTest(module=module.__name__), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                error = subprocess.TimeoutExpired(['test'], 1, output=b'partial out', stderr=b'partial err')
                with patch.object(module.subprocess, 'run', side_effect=error):
                    with self.assertRaises(subprocess.TimeoutExpired):
                        if module is compiler_load:
                            module.collect(Path('/unused'), root / 'out')
                        else:
                            module.captured(['test'], root, 'timeout', {}, 1)
                if module is compiler_load:
                    folder = root / 'out/generics-64'
                    stdout, stderr = folder / 'stdout', folder / 'stderr'
                    record = json.loads((root / 'out/attempts.json').read_text())[0]
                    self.assertTrue(record['timeout'])
                else:
                    stdout, stderr = root / 'timeout.stdout', root / 'timeout.stderr'
                    record = json.loads((root / 'timeout.command.json').read_text())
                    self.assertTrue(record['timed_out'])
                self.assertEqual(stdout.read_bytes(), b'partial out')
                self.assertEqual(stderr.read_bytes(), b'partial err')
                self.assertIn('failure', record)

    def test_malformed_compiler_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / 'out'
            result = subprocess.CompletedProcess(['test'], 1, b'not json', b'diagnostic')
            with patch.object(compiler_load.subprocess, 'run', return_value=result):
                with self.assertRaises(json.JSONDecodeError):
                    compiler_load.collect(Path('/unused'), out)
            record = json.loads((out / 'attempts.json').read_text())[0]
            self.assertEqual(record['exit_status'], 1)
            self.assertIn('failure', record)
            self.assertEqual((out / 'generics-64/stdout').read_bytes(), b'not json')


if __name__ == '__main__':
    unittest.main()
