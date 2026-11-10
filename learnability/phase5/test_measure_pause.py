import json
import unittest
from measure_pause import STAGES, parse_records


class Parsing(unittest.TestCase):
    def records(self):
        return [dict(mo_pause=1, attempt=1, stage=s, outcome=o, elapsed_ns=1)
                for s, o in STAGES.items()]

    def parse(self, records):
        return parse_records('\n'.join(json.dumps(r, separators=(',', ':')) for r in records))

    def test_complete(self):
        self.assertEqual(len(self.parse(self.records())), len(STAGES))

    def test_explicit_migration_rejected(self):
        for stage in ('old_decode', 'migration_call'):
            records = self.records()
            records.append(dict(mo_pause=1, attempt=1, stage=stage,
                                outcome='ok', elapsed_ns=1))
            with self.assertRaises(ValueError):
                self.parse(records)

    def test_duplicate(self):
        for record in self.records():
            with self.assertRaises(ValueError):
                self.parse(self.records() + [record])

    def test_missing(self):
        for i in range(len(STAGES)):
            records = self.records()
            del records[i]
            with self.assertRaises(ValueError):
                self.parse(records)

    def test_null_and_refusal(self):
        for field, value in [('elapsed_ns', None), ('outcome', 'refused')]:
            for i in range(len(STAGES)):
                records = self.records()
                records[i][field] = value
                with self.assertRaises(ValueError):
                    self.parse(records)

class SnapshotValidation(unittest.TestCase):
    def test_exact_snapshot_matches_readback_and_rejects_corruption(self):
        import os
        import pathlib
        import select
        import subprocess
        import tempfile
        import measure_pause as m
        binary_name = os.environ.get('MO_MEASUREMENT_BINARY')
        if not binary_name:
            self.skipTest('set MO_MEASUREMENT_BINARY for real snapshot controls')
        binary = pathlib.Path(binary_name)
        env = {**os.environ, 'QUEUE_TOKEN': 'PauseMeasurementServiceCanary',
               'QUEUE_UPGRADE': 'PauseMeasurementUpgradeCanary'}
        for corruption in (None, 'payload', 'next_id', 'attempts', 'deadline'):
            with self.subTest(corruption=corruption), tempfile.TemporaryDirectory() as tmp:
                root = pathlib.Path(tmp)
                m.fixture(root / 'source', 10, 1)
                queue = root / 'source/job-queue.mo'
                text = queue.read_text()
                if corruption:
                    before, verifier = text.split('fn verify_fixture', 1)
                    if corruption == 'payload':
                        before = before.replace('"job-#{id}:"', '"bad-#{id}:"')
                    elif corruption == 'next_id':
                        before = before.replace('next_id: 11)', 'next_id: 12)')
                    elif corruption == 'attempts':
                        before = before.replace('attempts: if id <= 1 : 1 else: 0', 'attempts: 2')
                    else:
                        before = before.replace('2099-01-01', '2098-01-01')
                    queue.write_text(before + 'fn verify_fixture' + verifier)
                (root / 'data').mkdir()
                with (root / 'serve.stderr').open('wb') as errors:
                    child = subprocess.Popen([str(binary), 'serve', str(root / 'source/main.mo'),
                        '--port', '0', '--data', str(root / 'data')], env=env,
                        stdout=subprocess.PIPE, stderr=errors, bufsize=0)
                    try:
                        self.assertTrue(select.select([child.stdout], [], [], 60)[0])
                        port = int(child.stdout.readline().split(b':')[-1].split(b'/')[0])
                        if corruption is None:
                            jobs = m.read_jobs(port, 10, 1)
                            self.assertEqual(len(jobs), 10)
                            self.assertEqual(m.verify_live(port, root, 'live')['structuredContent'], {'value': True})
                            proof = m.verify_snapshot(binary, port, root / 'source/main.mo', root, 'state', env)
                            self.assertTrue(proof['query']['result']['value'])
                        else:
                            with self.assertRaisesRegex(ValueError, 'fixture state differs'):
                                m.verify_live(port, root, 'live')
                            with self.assertRaisesRegex(ValueError, 'fixture state differs'):
                                m.verify_snapshot(binary, port, root / 'source/main.mo', root, 'state', env)
                    finally:
                        child.terminate()
                        try:
                            child.wait(timeout=15)
                        except subprocess.TimeoutExpired:
                            child.kill()
                            child.wait()
                        child.stdout.close()


if __name__ == '__main__':
    unittest.main()
