#!/usr/bin/env python3
"""The publication scanner for the Phase 4 evaluation (learnability/phase4/protocol.md, section 10).

  python3 learnability/phase4/scan.py TOKENS_FILE... -- PATH...
        read every file under the PATHs and fail if one holds a token from the TOKENS_FILEs,
        in plain text or in an encoding the transports use, or if one cannot be read

Each TOKENS_FILE holds one token per line; blank lines are ignored and surrounding whitespace
is stripped. They are the harness's session-token file and the session user's provider
credentials (client OAuth and API tokens). The fixed canaries TaskQueueCanary42,
Phase4SizingCanary and Phase4ClientProbeCanary are exempt: a tokens file may list them, and
they are not searched for. Any other token shorter than 16 bytes is a usage error, since it
cannot be told apart from ordinary text.

For each token the scanner looks for its raw bytes; its base64 and base64url forms at each of
the three alignments (base64-K is the token preceded by K bytes of its 3-byte group, keeping
only the characters the token alone decides); its hexadecimal form in upper and lower case;
and the same nine forms of its first 16 bytes (prefix-raw, prefix-base64-1 and so on), since
a 16-byte prefix is a leak too. It looks in two views of each file: the raw bytes, and the
bytes after decoding %xx URL escapes and JSON escapes (\\uXXXX, surrogate pairs, and the
two-character escapes such as \\/), so a token escaped only in part is still found. A match
found only in the second view is reported with its encoding prefixed by "escaped:". A pattern
two encodings share is reported under each.

Files are read in 1 MiB chunks. Each view keeps the end of the previous chunk, one byte fewer
than the longest pattern, and the decoder holds back a possible escape cut by the chunk, so a
token or an escape split between chunks is found. No file is skipped for its size, and no
more than a chunk and its overlap is held at once. A file is refused, and not read, when its
name ends in .sqlite, .gz, .zst, .tar, .zip, .xz, .bz2 or .7z or it starts with one of those
formats' signatures or a zlib header, since such a file can hold a token the scanner cannot
see; when it is neither a regular file nor a directory; when it or its directory cannot be
read; and when its path holds a token, in which case the path is not printed.

Output is one line per hit, `PATH:OFFSET ENCODING TOKEN`, and one per refusal,
`PATH: refused: REASON`, then the count of files read: `scanned N files: clean`, or the counts
of hits and refusals. OFFSET is the byte offset in the file where the match starts; for an
escaped match it is the offset of the raw bytes that decode to the match's first byte, so it
is exact in both views. TOKEN is the token's position among the non-blank lines of the
TOKENS_FILEs, in the order given, counting from 1, canaries included. No token, nor any part
of one, is printed. The exit status is 0 when every file was read and nothing was found, 1 on
any hit or refusal, and 2 for a usage error, which includes a tokens file that cannot be read.
"""

import array
import base64
import bisect
import os
import re
import stat
import sys

CHUNK = 1 << 20
PREFIX = 16
CANARIES = {b"TaskQueueCanary42", b"Phase4SizingCanary", b"Phase4ClientProbeCanary"}
REFUSED_SUFFIXES = (".sqlite", ".gz", ".zst", ".tar", ".zip", ".xz", ".bz2", ".7z")
SIGNATURES = [(0, b"SQLite format 3\x00", "an SQLite database"), (0, b"\x1f\x8b", "gzip data"),
              (0, b"\x28\xb5\x2f\xfd", "zstd data"), (257, b"ustar", "a tar archive"),
              (0, b"PK\x03\x04", "a zip archive"), (0, b"PK\x05\x06", "a zip archive"),
              (0, b"\xfd7zXZ\x00", "xz data"), (0, b"BZh", "bzip2 data"),
              (0, b"7z\xbc\xaf\x27\x1c", "a 7z archive")]
# zlib streams, git's loose objects among them, start with 0x78 and a second byte that makes
# the pair a multiple of 31.
ZLIB_SECOND = (0x01, 0x5E, 0x9C, 0xDA)

# A surrogate pair, then one \u escape, a %xx escape and a two-character JSON escape. The
# longest, the pair, is 12 bytes; the decoder holds back that many at a chunk's end.
ESCAPE = re.compile(rb"\\u([Dd][89ABab][0-9A-Fa-f]{2})\\u([Dd][C-Fc-f][0-9A-Fa-f]{2})"
                    rb"|\\u([0-9A-Fa-f]{4})|%([0-9A-Fa-f]{2})|\\([\"\\/bfnrt])")
LONGEST_ESCAPE = 12
SHORT = {b'"': b'"', b"\\": b"\\", b"/": b"/", b"b": b"\b", b"f": b"\f", b"n": b"\n",
         b"r": b"\r", b"t": b"\t"}


def inner(encode, data, k):
    """The characters of encode(K bytes + data) that data alone decides: from the first whose
    six bits all come from data to the last that does not reach past it."""
    text = encode(b"\0" * k + data)
    return text[(8 * k + 5) // 6:8 * (k + len(data)) // 6]


def forms(data):
    """Each encoding's name and the bytes data takes in it."""
    yield "raw", data
    for name, encode in [("base64", base64.b64encode), ("base64url", base64.urlsafe_b64encode)]:
        for k in range(3):
            yield f"{name}-{k}", inner(encode, data, k)
    yield "hex-upper", data.hex().upper().encode()
    yield "hex-lower", data.hex().encode()


def patterns(tokens):
    """{pattern: [(token position, encoding, is a prefix)]} for every token's every form."""
    table = {}
    for index, token in tokens:
        for prefix, data in [(False, token), (True, token[:PREFIX])]:
            for encoding, form in forms(data):
                table.setdefault(form, []).append((index, encoding, prefix))
    return table


def search(table, buffer, fresh):
    """(start, labels) for every match in buffer that ends past buffer[fresh], the first byte
    not searched before."""
    for form, labels in table.items():
        at = buffer.find(form, max(0, fresh - len(form) + 1))
        while at >= 0:
            yield at, labels
            at = buffer.find(form, at + 1)


def replacement(match):
    high, low, unit, byte, short = match.groups()
    if high:
        code = 0x10000 + ((int(high, 16) - 0xD800) << 10) + (int(low, 16) - 0xDC00)
        return chr(code).encode()
    if unit:
        return chr(int(unit, 16)).encode("utf-8", "surrogatepass")
    if byte:
        return bytes([int(byte, 16)])
    return SHORT[short]


def decode(data, final, marks=None, out_at=0, in_at=0):
    """Decode the escapes in data. Unless final, stop before any escape that could still be
    cut by the end of data. Returns the decoded bytes and how many bytes of data they use.
    Each escape's decoded and raw extents, offset by out_at and in_at, go into marks."""
    limit = len(data) if final else max(0, len(data) - LONGEST_ESCAPE)
    out = bytearray()
    pos = 0
    for match in ESCAPE.finditer(data):
        if match.end() > limit:
            limit = min(match.start(), limit)
            break
        out += data[pos:match.start()]
        start = len(out)
        out += replacement(match)
        if marks is not None:
            extents = (start, len(out), match.start(), match.end())
            for column, value, base in zip(marks, extents, (out_at, out_at, in_at, in_at)):
                column.append(base + value)
        pos = match.end()
    out += data[pos:limit]
    return bytes(out), limit


class Escaped:
    """A file's second view, decoded chunk by chunk, with the map back to raw offsets."""

    def __init__(self):
        self.pending = b""   # raw bytes held back: a possible escape cut by the chunk
        self.pending_at = 0  # their offset in the file
        self.view = b""      # the decoded bytes kept from the last chunk and the new ones
        self.view_at = 0     # the decoded offset of view[0]
        # Each kept escape's decoded start and end and raw start and end, as offsets.
        self.marks = [array.array("q") for _ in range(4)]
        self.shift = 0       # raw minus decoded offset before the first kept escape

    def feed(self, chunk, final, keep):
        """Decode chunk after the last keep decoded bytes; returns where the new bytes start."""
        kept = self.view[max(0, len(self.view) - keep):] if keep else b""
        self.view_at += len(self.view) - len(kept)
        _, out_ends, _, in_ends = self.marks
        gone = bisect.bisect_right(out_ends, self.view_at)
        if gone:
            self.shift = in_ends[gone - 1] - out_ends[gone - 1]
            for column in self.marks:
                del column[:gone]
        data = self.pending + chunk
        out, used = decode(data, final, self.marks, self.view_at + len(kept), self.pending_at)
        self.pending, self.pending_at = data[used:], self.pending_at + used
        self.view = kept + out
        return len(kept)

    def raw_offset(self, at):
        """The raw offset of the byte that decodes to view[at], or of its escape."""
        decoded = self.view_at + at
        out_starts, out_ends, in_starts, in_ends = self.marks
        i = bisect.bisect_right(out_starts, decoded) - 1
        if i < 0:
            return decoded + self.shift
        if decoded < out_ends[i]:
            return in_starts[i]
        return in_ends[i] + decoded - out_ends[i]


def path_leaks(table, path):
    """The positions of the tokens a path holds, raw or decoded."""
    data = os.fsencode(path)
    decoded, _ = decode(data, True)
    return sorted({label[0] for form, labels in table.items() if form in data or form in decoded
                   for label in labels})


def scan_file(path, table, keep):
    """{(offset, token, encoding): (is a prefix, is escaped)} for a file, or a refusal."""
    hits = {}

    def record(offset, labels, escaped):
        for index, encoding, prefix in labels:
            key, rank = (offset, index, encoding), (prefix, escaped)
            if key not in hits or rank < hits[key]:
                hits[key] = rank

    try:
        with open(path, "rb") as stream:
            raw, raw_at, escaped = b"", 0, Escaped()
            first = True
            while True:
                chunk = stream.read(CHUNK)
                if first:
                    first = False
                    for at, signature, what in SIGNATURES:
                        if chunk[at:at + len(signature)] == signature:
                            return f"{what}, not scanned inside"
                    if chunk[:1] == b"\x78" and chunk[1:2] and chunk[1] in ZLIB_SECOND:
                        return "zlib data, not scanned inside"
                kept = raw[max(0, len(raw) - keep):] if raw else b""
                raw_at += len(raw) - len(kept)
                raw = kept + chunk
                for at, labels in search(table, raw, len(kept)):
                    record(raw_at + at, labels, False)
                fresh = escaped.feed(chunk, not chunk, keep)
                for at, labels in search(table, escaped.view, fresh):
                    record(escaped.raw_offset(at), labels, True)
                if not chunk:
                    return hits
    except OSError as error:
        return f"cannot read: {error.strerror or error}"


def walk(path, seen):
    """(path, refusal or None) for each file under path, refusing what cannot be listed."""
    try:
        status = os.stat(path)
    except OSError as error:
        yield path, f"cannot read: {error.strerror or error}"
        return
    if stat.S_ISDIR(status.st_mode):
        if (status.st_dev, status.st_ino) in seen:
            return
        seen.add((status.st_dev, status.st_ino))
        try:
            names = sorted(os.listdir(path))
        except OSError as error:
            yield path, f"cannot read: {error.strerror or error}"
            return
        for name in names:
            yield from walk(os.path.join(path, name), seen)
    elif not stat.S_ISREG(status.st_mode):
        yield path, "neither a regular file nor a directory"
    elif os.path.basename(path).lower().endswith(REFUSED_SUFFIXES):
        yield path, f"a {os.path.splitext(path)[1].lower()} file, not scanned inside"
    else:
        yield path, None


def read_tokens(names):
    """[(position, token)] for every token that is not a canary, and how many lines there are."""
    tokens, position = [], 0
    for name in names:
        try:
            with open(name, "rb") as stream:
                lines = stream.read().splitlines()
        except OSError as error:
            raise ValueError(f"cannot read tokens file {name}: {error.strerror or error}")
        for line in lines:
            line = line.strip()
            if not line:
                continue
            position += 1
            if line in CANARIES:
                continue
            if len(line) < PREFIX:
                raise ValueError(f"token {position} is shorter than {PREFIX} bytes")
            tokens.append((position, line))
    if position == 0:
        raise ValueError("the tokens files hold no tokens")
    return tokens


def main(args):
    if "--" not in args:
        raise ValueError("usage: scan.py TOKENS_FILE... -- PATH...")
    split = args.index("--")
    names, paths = args[:split], args[split + 1:]
    if not names or not paths:
        raise ValueError("usage: scan.py TOKENS_FILE... -- PATH...")
    tokens = read_tokens(names)
    table = patterns(tokens)
    keep = max(map(len, table), default=1) - 1
    scanned = hit_count = refused = 0
    seen = set()
    for top in paths:
        for path, refusal in walk(top, seen):
            leaks = path_leaks(table, path)
            if leaks:
                refused += 1
                print(f"(path not shown): refused: the path holds token {leaks[0]}")
                continue
            result = scan_file(path, table, keep) if refusal is None else refusal
            if isinstance(result, str):
                refused += 1
                print(f"{path}: refused: {result}")
                continue
            scanned += 1
            for (offset, index, encoding), (prefix, escaped) in sorted(result.items()):
                hit_count += 1
                label = ("escaped:" if escaped else "") + ("prefix-" if prefix else "") + encoding
                print(f"{path}:{offset} {label} {index}")
    if hit_count or refused:
        print(f"scanned {scanned} files: {hit_count} hits, {refused} refused")
        return 1
    print(f"scanned {scanned} files: clean")
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(errors="backslashreplace")
    try:
        sys.exit(main(sys.argv[1:]))
    except ValueError as error:
        print(f"scan.py: {error}", file=sys.stderr)
        sys.exit(2)
