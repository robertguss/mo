import base64
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
import publish


class Publication(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.tokens = [b'service-secret-long-0123456789', b'upgrade-secret-long-9876543210',
                       b'provider-secret-long-5555555555']
        self.secret_file = self.root / 'secrets'
        self.secret_file.write_bytes(b'\n'.join(self.tokens[:2]))
        self.provider_file = self.root / 'provider-secrets'
        self.provider_file.write_bytes(self.tokens[2])
        self.sessions = []
        for slot, client in enumerate(publish.evidence.ORDER, 1):
            directory = self.root / str(slot)
            directory.mkdir()
            (directory / 'manifest.json').write_text(json.dumps({
                'collection_id': format(slot,'032x'), 'started_at': float(slot),
                'slot': slot, 'client': client, 'development': False,
                'correct': slot != 4, 'launched': True, 'timeout': slot == 4,
                'dimensions': dict.fromkeys((*publish.DIMENSIONS, 'publication_integrity'), True), 'failures': ['timeout'] if slot == 4 else []}))
            (directory / 'stream.jsonl').write_text('{}\n')
            (directory / 'proxy.jsonl').write_text('{}\n')
            self.sessions.append(directory)

    def test_missing_dimensions_cannot_select_legacy_credit(self):
        for directory in self.sessions:
            path = directory / 'manifest.json'
            row = json.loads(path.read_text())
            del row['dimensions']
            path.write_text(json.dumps(row))
        calls = []
        def regrade(directory):
            calls.append(directory)
            return dict.fromkeys(publish.DIMENSIONS, False)
        with self.assertRaisesRegex(ValueError, 'dimensions required'):
            publish.publish(self.sessions, self.root / 'downgrade',
                            [self.secret_file], [self.provider_file],
                            protected_regrade=regrade)
        self.assertFalse((self.root / 'downgrade').exists())

    def test_protected_classification_and_binding_required(self):
        for value in ({'evidence_class': 'DEVELOPMENT'}, {'slot': 4}, {'client': 'claude'}):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, 'protected classification'):
                publish.publish(self.sessions, self.root / 'forged-class',
                    [self.secret_file], [self.provider_file],
                    protected_regrade=lambda directory: dict(self.record(directory), **value))
        self.assertFalse((self.root / 'forged-class').exists())

    def record(self, directory, dimensions=None):
        slot = self.sessions.index(directory) + 1
        return dict(evidence_class='FINAL', slot=slot, client=publish.evidence.ORDER[slot-1],
                    collection_id=format(slot,'032x'), started_at=float(slot), execution_clean=True,
                    dimensions=dict.fromkeys(publish.DIMENSIONS, True) if dimensions is None else dimensions)

    def test_reassembled_collection_is_refused(self):
        for change in ({'collection_id':'f'*32}, {'started_at':999.0}):
            with self.subTest(change=change), self.assertRaisesRegex(ValueError, 'collection binding'):
                publish.publish(self.sessions, self.root/'mixed', [self.secret_file],
                    [self.provider_file], protected_regrade=lambda d: dict(self.record(d), **change))
        result = publish.publish(self.sessions, self.root/'unclean', [self.secret_file],
            [self.provider_file], protected_regrade=lambda d: dict(self.record(d), execution_clean=False))
        self.assertFalse(result['gate']['passed'])

    def test_dimensions_require_independent_regrade(self):
        for directory in self.sessions:
            path = directory / 'manifest.json'
            row = json.loads(path.read_text())
            row.update(dimensions=dict.fromkeys(publish.DIMENSIONS, True),
                       correct=False, launched=True, timeout=False, failures=[])
            row['dimensions']['publication_integrity'] = False
            path.write_text(json.dumps(row))
        with self.assertRaisesRegex(ValueError, 'independent protected'):
            publish.publish(self.sessions, self.root / 'refused', [self.secret_file], [self.provider_file])
        with self.assertRaisesRegex(ValueError, 'complete boolean'):
            publish.publish(self.sessions, self.root / 'missing', [self.secret_file], [self.provider_file],
                            protected_regrade=lambda directory: self.record(directory, {'task_state': True}))
        # Synthetic regrader exercises the interface only; it is not qualification.
        result = publish.publish(self.sessions, self.root / 'dimensions',
                                 [self.secret_file], [self.provider_file],
                                 protected_regrade=self.record)
        self.assertTrue(result['gate']['passed'])
        result = publish.publish(self.sessions, self.root / 'failed-dimension',
                                 [self.secret_file], [self.provider_file],
                                 protected_regrade=lambda directory: self.record(directory, dict(dict.fromkeys(publish.DIMENSIONS, True), model_provenance=False)))
        self.assertFalse(result['gate']['passed'])

    def test_regraded_pass_requires_original_logs(self):
        for directory in self.sessions:
            path = directory / 'manifest.json'
            row = json.loads(path.read_text())
            row.update(dimensions=dict.fromkeys(publish.DIMENSIONS, True),
                       correct=False, launched=True, timeout=False, failures=[])
            row['dimensions']['publication_integrity'] = False
            path.write_text(json.dumps(row))
        for name in ('stream.jsonl', 'proxy.jsonl'):
            path = self.sessions[0] / name
            original = path.read_bytes()
            path.unlink()
            with self.subTest(name=name), self.assertRaises((ValueError, FileNotFoundError)):
                publish.publish(self.sessions, self.root / ('missing-' + name),
                                [self.secret_file], [self.provider_file],
                                protected_regrade=self.record)
            path.write_bytes(original)

    def test_scanner_rejects_unredacted_credentials(self):
        table = publish.scan.patterns(publish.scan.read_tokens([self.secret_file, self.provider_file]))
        for token in self.tokens:
            for encoded in (token, base64.b64encode(token), token.hex().encode()):
                (self.sessions[0] / 'leak.txt').write_bytes(encoded)
                with self.assertRaises(ValueError):
                    publish.scan_tree(self.sessions[0], table)

    def tearDown(self):
        self.temp.cleanup()

    def test_all_slots_misses_encodings_and_archive(self):
        encodings = []
        for token in self.tokens:
            encodings.extend([token.decode(), base64.b64encode(token).decode(),
                              token.hex(), ''.join('%%%02x' % b for b in token),
                              ''.join('\\u%04x' % b for b in token)])
        stream = self.sessions[0] / 'stream.jsonl'
        stream.write_text(''.join(json.dumps({'text': text, 'kept': 1}) + '\n' for text in encodings))
        original = stream.read_bytes()
        (self.sessions[1] / 'client-stderr.txt').write_text('\n'.join(encodings))
        (self.sessions[0] / 'source-tests.json').write_text(
            json.dumps({'record': 'test', 'detail': self.tokens[2].decode()}) + '\n' +
            json.dumps({'record': 'summary', 'passed': 47}) + '\n')
        result = publish.publish(self.sessions, self.root / 'out', [self.secret_file], [self.provider_file],
                                 protected_regrade=lambda directory: self.record(directory, dict.fromkeys(publish.DIMENSIONS, directory != self.sessions[3])))
        self.assertTrue(result['publication_scan_clean'])
        self.assertTrue(result['gate']['passed'])
        self.assertEqual(result['gate']['phase'], 'open')
        self.assertFalse(result['uploaded'])
        self.assertEqual(original, stream.read_bytes())
        staged = self.root / 'out/stage'
        self.assertEqual(len(list(staged.glob('slot-*'))), 4)
        miss = json.loads((staged / 'slot-4/manifest.json').read_text())
        self.assertEqual(miss['failures'], ['timeout'])
        self.assertFalse(miss['correct'])
        cleaned = [json.loads(line) for line in (staged / 'slot-1/stream.jsonl').read_text().splitlines()]
        self.assertTrue(all(row['text'].startswith('[redacted') and row['kept'] == 1 for row in cleaned))
        self.assertEqual(result['stage_files_scanned'], result['extracted_files_scanned'])
        tests = [json.loads(line) for line in (staged / 'slot-1/source-tests.json').read_text().splitlines()]
        self.assertEqual(tests[1], {'record': 'summary', 'passed': 47})
        self.assertTrue(tests[0]['detail'].startswith('[redacted'))

    def test_missing_provider_inventory_refused(self):
        self.provider_file.write_text('')
        with self.assertRaises(ValueError):
            publish.publish(self.sessions, self.root / 'out', [self.secret_file], [self.provider_file])
        self.assertFalse((self.root / 'out').exists())

    def test_missing_slot_and_development_refused(self):
        with self.assertRaises(ValueError):
            publish.publish(self.sessions[:3], self.root / 'out', [self.secret_file], [self.provider_file])
        path = self.sessions[0] / 'manifest.json'
        row = json.loads(path.read_text())
        row['development'] = True
        path.write_text(json.dumps(row))
        with self.assertRaises(ValueError):
            publish.publish(self.sessions, self.root / 'out', [self.secret_file], [self.provider_file])
        self.assertFalse((self.root / 'out').exists())

    def test_input_symlink_and_archive_attacks(self):
        (self.sessions[0] / 'service.log').symlink_to(self.secret_file)
        with self.assertRaises(ValueError):
            publish.publish(self.sessions, self.root / 'out', [self.secret_file], [self.provider_file])
        for index, (name, kind) in enumerate([('../escape', tarfile.REGTYPE),
                                             ('/absolute', tarfile.REGTYPE),
                                             ('link', tarfile.SYMTYPE),
                                             ('hard', tarfile.LNKTYPE)]):
            archive = self.root / f'attack-{index}.tar.gz'
            with tarfile.open(archive, 'w:gz') as out:
                item = tarfile.TarInfo(name)
                item.type = kind
                item.linkname = '/etc/passwd'
                item.size = 0
                out.addfile(item, io.BytesIO())
            with self.assertRaises(ValueError):
                publish.extract(archive, self.root / f'extract-{index}')
        self.assertFalse((self.root / 'escape').exists())

    def test_scanner_refuses_opaque_file(self):
        tree = self.root / 'opaque'
        tree.mkdir()
        (tree / 'log.txt').write_bytes(b'SQLite format 3\x00')
        with self.assertRaises(ValueError):
            publish.scan_tree(tree, publish.scan.patterns(list(enumerate(self.tokens, 1))))


if __name__ == '__main__':
    unittest.main()
