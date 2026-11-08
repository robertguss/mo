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
                'slot': slot, 'client': client, 'development': False,
                'correct': slot != 4, 'failures': ['timeout'] if slot == 4 else []}))
            (directory / 'stream.jsonl').write_text('{}\n')
            (directory / 'proxy.jsonl').write_text('{}\n')
            self.sessions.append(directory)

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
        result = publish.publish(self.sessions, self.root / 'out', [self.secret_file], [self.provider_file])
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
