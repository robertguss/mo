#!/usr/bin/env python3
"""Tests for learnability/phase4/scan.py, the publication scanner, on seeded leaks in each
encoding the transports use. Every run's output is checked for the tokens themselves.

Run: python3 learnability/phase4/test_scan.py
"""

import base64
import json
import os
import pathlib
import re
import secrets
import subprocess
import sys
import tempfile
import unittest
import urllib.parse

here = pathlib.Path(__file__).resolve().parent
SCAN = here / "scan.py"
PREFLIGHT = here / "preflight" / "results"
CHUNK = 1 << 20
CANARIES = ["TaskQueueCanary42", "Phase4SizingCanary", "Phase4ClientProbeCanary"]
HIT = re.compile(r"^(.*):(\d+) (\S+) (\d+)$")
REFUSED = re.compile(r"^(.*): refused: (.+)$")

# Every token a test makes, so every run's output can be checked for any piece of one.
SECRETS = []


def token():
    """A session token as the harness makes one: 32 random bytes, base64url, 43 characters."""
    made = secrets.token_urlsafe(32).encode()
    SECRETS.append(made)
    return made


def slashed_token():
    """A token whose base64 differs from its base64url at every alignment: a run of '?'
    (0x3F) puts a '/' in its base64 whatever the alignment, where base64url has '_'."""
    made = secrets.token_urlsafe(16).encode() + b"????????" + secrets.token_urlsafe(16).encode()
    SECRETS.append(made)
    return made


def leaked(output):
    """The first piece of a token found in output, described without the token, or None."""
    for index, secret in enumerate(SECRETS):
        pieces = [secret[i:i + 16] for i in range(len(secret) - 15)]
        pieces += [secret.hex().encode(), secret.hex().upper().encode(),
                   secret[:16].hex().encode(), secret[:16].hex().upper().encode(),
                   base64.b64encode(secret)[:20], base64.urlsafe_b64encode(secret)[:20]]
        for piece in pieces:
            if piece in output:
                return f"a piece of secret {index}"
    return None


class Case(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name)

    def tearDown(self):
        for path in [self.root, *self.root.rglob("*")]:
            if not path.is_symlink():
                try:
                    os.chmod(path, 0o700)
                except OSError:
                    pass
        self.temporary.cleanup()

    def tokens(self, *lines, name="tokens"):
        path = self.root / name
        path.write_bytes(b"".join((line if isinstance(line, bytes) else line.encode()) + b"\n"
                                  for line in lines))
        os.chmod(path, 0o600)
        return path

    def write(self, name, content):
        path = self.root / "publish" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content if isinstance(content, bytes) else content.encode())
        return path

    def scan(self, *args):
        result = subprocess.run([sys.executable, str(SCAN), *map(str, args)],
                                capture_output=True, check=False, timeout=300)
        self.assertIsNone(leaked(result.stdout + result.stderr), "the scanner printed a token")
        return result

    def hits(self, result):
        """{file name: set of (offset, encoding, token)} from a run's hit lines."""
        found = {}
        for line in result.stdout.decode().splitlines():
            match = HIT.match(line)
            if match:
                name = pathlib.Path(match[1]).name
                found.setdefault(name, set()).add((int(match[2]), match[3], int(match[4])))
        return found

    def refusals(self, result):
        """{path: reason} from a run's refusal lines."""
        return {m[1]: m[2] for m in map(REFUSED.match, result.stdout.decode().splitlines()) if m}


def encodings(found):
    return {encoding for _, encoding, _ in found}


def url_all(data):
    return "".join(f"%{b:02X}" for b in data).encode()


def json_all(data):
    return "".join(f"\\u{b:04x}" for b in data).encode()


class Leaks(Case):
    def test_proxy_log_with_a_base64_token_fails_and_names_the_file(self):
        secret = token()
        tokens = self.tokens(secret)
        clean = json.dumps({"seq": 1, "direction": "request", "path": "/mcp"}) + "\n"
        blob = base64.b64encode(b"user:" + secret).decode()
        line = json.dumps({"seq": 2, "headers": {"authorization": "Basic " + blob}})
        log = self.write("traffic.jsonl", clean + line + "\n")
        result = self.scan(tokens, "--", log)
        self.assertEqual(result.returncode, 1)
        # "user:" is 5 bytes, so the token starts 2 bytes into a 3-byte group, and the first
        # base64 character it alone decides is the 7th: ceil(8 * 5 / 6).
        offset = len(clean) + line.index(blob) + 7
        self.assertIn(f"{log}:{offset} base64-2 1", result.stdout.decode().splitlines())

    def test_one_seeded_leak_per_encoding(self):
        secret, slashed = token(), slashed_token()
        tokens = self.tokens(secret, slashed)

        def blob(encode, data, k):
            return encode(os.urandom(30 + k) + data + os.urandom(7))

        seeded = {
            "raw.txt": (b"Bearer " + secret + b"\n", "raw", 1),
            "url-full.log": (b"GET /?t=" + url_all(secret) + b"\n", "escaped:raw", 1),
            "url-part.log": (b"t=" + b"".join(b"%%%02x" % c if i % 2 else bytes([c])
                                             for i, c in enumerate(secret)), "escaped:raw", 1),
            "json-full.json": (b'{"t": "' + json_all(secret) + b'"}', "escaped:raw", 1),
            "json-part.json": (b'{"t": "' + secret[:5] + json_all(secret[5:9]) + secret[9:]
                               + b'"}', "escaped:raw", 1),
            "hex-upper.txt": (secret.hex().upper().encode(), "hex-upper", 1),
            "hex-lower.txt": (secret.hex().encode(), "hex-lower", 1),
            "prefix-raw.txt": (b"token " + secret[:16] + b"...\n", "prefix-raw", 1),
            "prefix-hex.txt": (b"id=" + secret[:16].hex().encode(), "prefix-hex-lower", 1),
            "prefix-b64.txt": (blob(base64.b64encode, secret[:16], 1), "prefix-base64-1", 1),
        }
        for k in range(3):
            seeded[f"b64-{k}.txt"] = (blob(base64.b64encode, secret, k), f"base64-{k}", 1)
            seeded[f"b64-slashed-{k}.txt"] = (blob(base64.b64encode, slashed, k),
                                              f"base64-{k}", 2)
            seeded[f"b64url-{k}.txt"] = (blob(base64.urlsafe_b64encode, slashed, k),
                                         f"base64url-{k}", 2)
            quoted = urllib.parse.quote(blob(base64.b64encode, slashed, k), safe="").encode()
            seeded[f"b64-quoted-{k}.txt"] = (b"q=" + quoted, f"escaped:base64-{k}", 2)
            text = blob(base64.b64encode, slashed, k).decode()
            seeded[f"b64-json-{k}.json"] = (json.dumps({"b": text}).replace("/", "\\/").encode(),
                                            f"escaped:base64-{k}", 2)
        for name, (content, _, _) in seeded.items():
            self.write(name, content)
        result = self.scan(tokens, "--", self.root / "publish")
        self.assertEqual(result.returncode, 1)
        found = self.hits(result)
        for name, (_, encoding, index) in seeded.items():
            self.assertIn((encoding, index), {(e, i) for _, e, i in found.get(name, ())}, name)
        # The base64 and base64url forms of the slashed token differ, so each is found only
        # under its own name, and a prefix is not reported where the whole token is.
        for k in range(3):
            self.assertNotIn(f"base64url-{k}", encodings(found[f"b64-slashed-{k}.txt"]))
            self.assertNotIn(f"base64-{k}", encodings(found[f"b64url-{k}.txt"]))
        self.assertNotIn("raw", encodings(found["prefix-raw.txt"]))
        self.assertNotIn("prefix-raw", encodings(found["raw.txt"]))

    def test_a_raw_token_is_one_line_at_its_offset(self):
        secret = token()
        path = self.write("note.md", b"# notes\nthe token was " + secret + b" today\n")
        result = self.scan(self.tokens(secret), "--", path)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout.decode().splitlines(),
                         [f"{path}:22 raw 1", "scanned 1 files: 1 hits, 0 refused"])

    def test_a_provider_credential_from_the_second_tokens_file_fails(self):
        session, credential = token(), token()
        first = self.tokens(CANARIES[0], session, name="session-tokens")
        second = self.tokens("", credential, name="provider-tokens")
        events = [{"type": "auth", "client": "codex"},
                  {"type": "request", "headers": {"x-api-key": credential.decode()}}]
        stream = self.write("events.jsonl", "".join(json.dumps(e) + "\n" for e in events))
        result = self.scan(first, second, "--", stream)
        self.assertEqual(result.returncode, 1)
        # Token positions count every non-blank line, canaries included, from 1.
        self.assertEqual({(e, i) for _, e, i in self.hits(result)["events.jsonl"]}, {("raw", 3)})

    def test_a_token_in_a_path_is_refused_without_printing_the_path(self):
        secret = token()
        path = self.write("session-" + secret.decode() + ".log", "nothing here\n")
        result = self.scan(self.tokens(secret), "--", path)
        self.assertEqual(result.returncode, 1)
        self.assertIn("the path holds token 1", result.stdout.decode())


class Boundaries(Case):
    def test_a_token_split_across_chunks_is_found(self):
        secret = token()
        content = b"x" * (CHUNK - 20) + secret + b"y" * (CHUNK - 3) + secret + b"z" * 9
        path = self.write("big.log", content)
        result = self.scan(self.tokens(secret), "--", path)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.hits(result)["big.log"],
                         {(CHUNK - 20, "raw", 1), (2 * CHUNK + 20, "raw", 1)})

    def test_an_escape_split_across_chunks_still_decodes(self):
        secret = token()
        tokens = self.tokens(secret)
        # Cut each escaped form at every point inside its first escape, so the chunk
        # boundary falls after '%', after '%X', after '\', after '\u', after '\u0' and so on.
        for form, width in [(url_all, 3), (json_all, 6)]:
            for cut in range(1, width + 1):
                with self.subTest(form=form.__name__, cut=cut):
                    path = self.write(f"{form.__name__}-{cut}.log",
                                      b"x" * (CHUNK - cut) + form(secret) + b"\n")
                    result = self.scan(tokens, "--", path)
                    self.assertEqual(result.returncode, 1)
                    self.assertEqual(self.hits(result)[path.name],
                                     {(CHUNK - cut, "escaped:raw", 1)})

    def test_escapes_in_earlier_chunks_do_not_move_later_offsets(self):
        # Each %41 decodes to one byte, so the decoded view runs 2 bytes behind the file for
        # each; an escaped match two chunks later must still map to its offset in the file.
        secret = token()
        head = b"%41" * 1000 + b"x" * (2 * CHUNK)
        path = self.write("shifted.log", head + secret + b" " + url_all(secret) + b"\n")
        result = self.scan(self.tokens(secret), "--", path)
        self.assertEqual(self.hits(result)["shifted.log"],
                         {(len(head), "raw", 1), (len(head) + len(secret) + 1, "escaped:raw", 1)})

    def test_a_token_at_the_start_and_the_end_is_found(self):
        secret = token()
        path = self.write("edges.txt", secret + b"-" * 100 + secret)
        result = self.scan(self.tokens(secret), "--", path)
        self.assertEqual(self.hits(result)["edges.txt"],
                         {(0, "raw", 1), (len(secret) + 100, "raw", 1)})


class Canaries(Case):
    def test_canaries_listed_as_tokens_are_not_searched_for(self):
        canary_text = "".join(json.dumps({"token": c}) + "\n" for c in CANARIES)
        path = self.write("probe.jsonl", canary_text)
        for tokens in [self.tokens(*CANARIES, name="only-canaries"),
                       self.tokens(CANARIES[1], token(), CANARIES[2], CANARIES[0])]:
            result = self.scan(tokens, "--", path)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertEqual(result.stdout, b"scanned 1 files: clean\n")


class Refusals(Case):
    def test_containers_are_refused_by_name_and_by_signature(self):
        tokens = self.tokens(token())
        names = ["state.sqlite", "log.gz", "LOG.GZ", "trace.zst", "bundle.tar"]
        for name in names:
            self.write(name, "plain text that holds nothing\n")
        self.write("state.db", b"SQLite format 3\x00" + b"\x00" * 100)
        self.write("data.bin", b"\x1f\x8b\x08\x00" + b"\x00" * 100)
        self.write("frames.bin", b"\x28\xb5\x2f\xfd" + b"\x00" * 100)
        self.write("archive", b"\x00" * 257 + b"ustar\x0000" + b"\x00" * 300)
        self.write("fine.txt", "fine\n")
        result = self.scan(tokens, "--", self.root / "publish")
        self.assertEqual(result.returncode, 1)
        refused = {pathlib.Path(p).name for p in self.refusals(result)}
        self.assertEqual(refused, set(names) | {"state.db", "data.bin", "frames.bin", "archive"})
        self.assertTrue(result.stdout.decode().endswith("scanned 1 files: 0 hits, 9 refused\n"))

    def test_what_cannot_be_read_is_refused_not_skipped(self):
        tokens = self.tokens(token())
        fine = self.write("fine.txt", "fine\n")
        missing = self.root / "publish" / "missing.txt"
        fifo = self.root / "publish" / "pipe"
        os.mkfifo(fifo)
        result = self.scan(tokens, "--", fine, missing, fifo)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(set(self.refusals(result)), {str(missing), str(fifo)})
        if os.geteuid() == 0:
            self.skipTest("root can read a file at mode 000")
        locked = self.write("locked.txt", "secret-free but unreadable\n")
        os.chmod(locked, 0)
        closed = self.root / "publish" / "closed"
        closed.mkdir()
        (closed / "inside.txt").write_text("hidden\n")
        os.chmod(closed, 0)
        result = self.scan(tokens, "--", locked, closed, fine)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(set(self.refusals(result)), {str(locked), str(closed)})
        self.assertIn("cannot read", self.refusals(result)[str(locked)])


class Clean(Case):
    def test_a_clean_set_prints_only_a_count(self):
        secret = token()
        self.write("a.txt", "hello\n")
        self.write("nested/deeper/b.jsonl", json.dumps({"t": "not the token"}) + "\n")
        self.write("big.log", os.urandom(CHUNK * 5 // 2).hex().encode()[:CHUNK * 5 // 2])
        self.write("escapes.json", '{"a": "\\u0041\\ud83d\\ude00 %41 %zz \\"q\\" \\\\"}\n')
        result = self.scan(self.tokens(secret), "--", self.root / "publish")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(result.stdout, b"scanned 4 files: clean\n")
        self.assertEqual(result.stderr, b"")

    def test_the_published_preflight_results_pass(self):
        files = sum(len(names) for _, _, names in os.walk(PREFLIGHT))
        self.assertGreater(files, 0)
        result = self.scan(self.tokens(token()), "--", PREFLIGHT)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(result.stdout.decode(), f"scanned {files} files: clean\n")


class Usage(Case):
    def test_usage_errors_exit_2(self):
        good = self.tokens(token())
        path = self.write("a.txt", "a\n")
        cases = [(), (good, path), ("--", path), (good, "--"),
                 (self.root / "absent", "--", path),
                 (self.root, "--", path),
                 (self.tokens("", "  ", name="empty"), "--", path),
                 (self.tokens("short-token", name="short"), "--", path)]
        for args in cases:
            result = self.scan(*args)
            self.assertEqual(result.returncode, 2, args)
            self.assertTrue(result.stderr.startswith(b"scan.py: "), args)
            self.assertEqual(result.stdout, b"", args)


def main():
    return 0 if unittest.main(exit=False).result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
