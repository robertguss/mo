#!/usr/bin/env python3
"""Check the trusted Unicode primitives against the pinned UCD 16.0 files."""
import concurrent.futures
import pathlib
import random
import re
import struct
import subprocess
import sys
import time

from generate_unicode16 import read_sources


def rows(text):
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if line:
            yield number, line


def string(codes):
    return "".join(chr(int(c, 16)) for c in codes.split())


def hex_text(text):
    return text.encode("utf-8").hex()


def properties(text, name):
    result = set()
    for _, row in rows(text):
        fields = [s.strip() for s in row.split(";")]
        if fields[1] == name:
            bounds = [int(s, 16) for s in fields[0].split("..")]
            result.update(range(bounds[0], bounds[-1] + 1))
    return result


def categories(sources):
    expected = bytearray(0x110000)
    start = None
    for _, row in rows(sources["UnicodeData.txt"]):
        fields = row.split(";")
        cp = int(fields[0], 16)
        if fields[1].endswith(", First>"):
            assert start is None
            start = cp
            continue
        if fields[1].endswith(", Last>"):
            assert start is not None
        else:
            assert start is None
            start = cp
        value = 1 if fields[2] in {"Lu", "Ll", "Lt", "Lm", "Lo"} else 2 if fields[2] == "Nd" else 0
        expected[start:cp + 1] = bytes([value]) * (cp + 1 - start)
        start = None
    assert start is None
    del expected[0xD800:0xE000]
    return expected


def fixtures(sources):
    cases = []

    def add(label, op, text, expected):
        cases.append((label, f"{op} {hex_text(text)}\n", expected))

    zero = properties(sources["DerivedCoreProperties.txt"], "Default_Ignorable_Code_Point")
    wide = properties(sources["EastAsianWidth.txt"], "W") | properties(sources["EastAsianWidth.txt"], "F")
    emoji = properties(sources["emoji/emoji-data.txt"], "Emoji_Presentation")
    regional = properties(sources["auxiliary/GraphemeBreakProperty.txt"], "Regional_Indicator")
    cased = properties(sources["DerivedCoreProperties.txt"], "Cased")
    ignorable = properties(sources["DerivedCoreProperties.txt"], "Case_Ignorable")
    variants = {int(row.split()[0], 16) for _, row in rows(sources["emoji/emoji-variation-sequences.txt"])}
    assigned, lower, upper = set(), {}, {}
    first = None
    for _, raw in rows(sources["UnicodeData.txt"]):
        fields = raw.split(";")
        assert len(fields) == 15
        cp = int(fields[0], 16)
        if fields[1].endswith(", First>"):
            assert first is None
            first = cp
            continue
        start = first if fields[1].endswith(", Last>") else cp
        first = None
        assigned.update(c for c in range(start, cp + 1) if not 0xD800 <= c <= 0xDFFF)
        if fields[2] == "Cc" or fields[2].startswith("M"):
            zero.update(range(start, cp + 1))
        if fields[12]:
            upper[cp] = chr(int(fields[12], 16))
        if fields[13]:
            lower[cp] = chr(int(fields[13], 16))
    assert first is None
    for _, raw in rows(sources["SpecialCasing.txt"]):
        fields = [s.strip() for s in raw.split(";")]
        if not fields[4]:
            cp = int(fields[0], 16)
            lower[cp], upper[cp] = string(fields[1]), string(fields[3])

    def width(cluster):
        answer = 0
        for i, ch in enumerate(cluster):
            cp = ord(ch)
            w = 0 if cp in zero else 2 if cp in wide else 1
            following = cluster[i+1:i+2]
            presentation = cp in emoji and cp not in regional
            presentation |= cp in variants and following == "\ufe0f"
            presentation |= ch == "\u20e3" and (cluster[i-1:i] in "#*0123456789" and i > 0 or
                            i > 1 and cluster[i-1] == "\ufe0f" and cluster[i-2] in "#*0123456789")
            answer = max(answer, 2 if presentation else w)
        return max(answer, 2 if sum(ord(ch) in regional for ch in cluster) == 2 else 0)

    grapheme_count = 0
    add("empty graphemes", "g", "", "")
    add("empty width", "w", "", "0")
    for number, row in rows(sources["auxiliary/GraphemeBreakTest.txt"]):
        tokens = row.split()
        assert tokens[0] == tokens[-1] == "÷" and len(tokens) % 2 == 1
        clusters, current = [], ""
        for i in range(0, len(tokens) - 1, 2):
            assert tokens[i] in {"÷", "×"}
            if tokens[i] == "÷" and current:
                clusters.append(current)
                current = ""
            current += chr(int(tokens[i+1], 16))
        clusters.append(current)
        text = "".join(clusters)
        add(f"GraphemeBreakTest:{number}", "g", text, ":".join(map(hex_text, clusters)))
        add(f"grapheme width:{number}", "w", text, str(sum(map(width, clusters))))
        grapheme_count += 1
    assert grapheme_count == 1093

    normalized, part_one, part = 0, set(), None
    for number, row in rows(sources["NormalizationTest.txt"]):
        if row.startswith("@Part"):
            part = row
            continue
        fields = row.split(";")
        assert len(fields) == 6 and fields[-1] == ""
        columns = list(map(string, fields[:5]))
        if part == "@Part1":
            assert len(columns[0]) == 1
            part_one.add(ord(columns[0]))
        for i, text in enumerate(columns):
            expected = columns[1] if i < 3 else columns[3]
            add(f"NormalizationTest:{number}:c{i+1}", "n", text, hex_text(expected))
        normalized += 1
    assert normalized == 19965
    # The file also requires identity for every assigned scalar absent in Part 1.
    # Separators prevent unintended composition between independent test scalars.
    identity = sorted(assigned - part_one)
    for i in range(0, len(identity), 1024):
        text = "".join(chr(c) + "|" for c in identity[i:i+1024])
        add(f"NormalizationTest identity:{i}", "n", text, hex_text(text))

    # Full mappings are read directly from the source rows, not generated C data.
    changed = sorted(set(lower) | set(upper))
    for cp in changed:
        text = chr(cp)
        add(f"lower:{cp:04x}", "l", text, hex_text(lower.get(cp, text)))
        add(f"upper:{cp:04x}", "u", text, hex_text(upper.get(cp, text)))
    unchanged = sorted(assigned - set(changed))
    for i in range(0, len(unchanged), 1024):
        text = "".join(chr(c) for c in unchanged[i:i+1024])
        add(f"lower identity:{i}", "l", text, hex_text(text))
        add(f"upper identity:{i}", "u", text, hex_text(text))

    # Independent, bounded context checks from Default Case Algorithms. The
    # straightforward scans here are on short test strings, never runtime text.
    def full_case(text, table, down):
        result = []
        for i, ch in enumerate(text):
            before = next((ord(c) in cased for c in reversed(text[:i]) if ord(c) not in ignorable), False)
            after = next((ord(c) in cased for c in text[i+1:] if ord(c) not in ignorable), False)
            result.append("ς" if down and ch == "Σ" and before and not after else table.get(ord(ch), ch))
        return "".join(result)
    rng = random.Random(1600)
    examples = ["ΟΣ", "ΟΣΟΣ", "ΟΣ'Α", "ΟΣ'", "Σ", "İIıi", "Straße ﬃ", "AΣ\u0345", "AΣ\u0345A",
                "I\u0307", "AΣ\U000e0100", "AΣ\U000e0100B"]
    # Both overlapping Cased/Case_Ignorable and ordinary ignorable context.
    examples += ["AΣ" + chr(cp) + suffix for cp in sorted(ignorable) for suffix in ["", "A"]]
    examples += [prefix + chr(cp) + "Σ" for cp in sorted(cased & ignorable) for prefix in ["", "A"]]
    examples += ["".join(rng.choice("ΣAαßİıﬃ .'\u0345\u0307") for _ in range(rng.randrange(128)))
                 for _ in range(256)]
    for i, text in enumerate(examples):
        for op, table in [("l", lower), ("u", upper)]:
            add(f"full case context:{i}:{op}", op, text, hex_text(full_case(text, table, op == "l")))
    # Width data at every explicitly assigned scalar and property-range edge.
    width_scalars = set(changed)
    for text in [sources["UnicodeData.txt"], sources["EastAsianWidth.txt"],
                 sources["DerivedCoreProperties.txt"], sources["emoji/emoji-data.txt"]]:
        for _, row in rows(text):
            bounds = row.split(";", 1)[0].strip().split("..")
            for cp in map(lambda s: int(s, 16), bounds):
                for c in [cp - 1, cp, cp + 1]:
                    if 0 <= c <= 0x10FFFF and not 0xD800 <= c <= 0xDFFF:
                        width_scalars.add(c)
    for cp in sorted(width_scalars):
        text = chr(cp)
        add(f"width scalar:{cp:04x}", "w", text, str(width(text)))
    for _, row in rows(sources["emoji/emoji-variation-sequences.txt"]):
        text = string(row.split(";", 1)[0])
        add("emoji variation:" + row.split(";", 1)[0], "w", text, str(width(text)))
    # Keycaps are sequences, not Emoji_Presentation scalar properties or
    # variation-sequence rows. Exercise every base, both valid spellings and
    # nearby non-keycap forms with independent expected widths.
    for base in "#*0123456789":
        for selector in ["", "\ufe0f"]:
            text = base + selector + "\u20e3"
            add(f"keycap:{base}:{selector!r}", "w", text, "2")
            add(f"keycap grapheme:{base}:{selector!r}", "g", text, hex_text(text))
        add(f"keycap text selector:{base}", "w", base + "\ufe0e\u20e3", "1")
    add("keycap mark alone", "w", "\u20e3", "0")
    add("spacing mark width policy", "w", "का", "1")
    for i, (text, expected) in enumerate([
        ("\x00\t\n\r\n\x1f\x7f\x80\x9f", "0"),
        ("a\tb\r\nc\x85d", "4"),
        ("🇺", "1"), ("🇺🇸", "2"), ("🇺🇸🇨", "3"), ("🇺🇸🇨🇦", "4"),
        ("🇺\ufe0f", "1"), ("🇺\u0308🇸", "2"), ("🇺\t🇸", "2"),
    ]):
        add(f"D238 width policy:{i}", "w", text, expected)
    return cases, f"{grapheme_count} grapheme vectors, {normalized} NFC rows (all five columns), full case/width data"


def corpus(sources, directory):
    directory.mkdir(parents=True, exist_ok=True)
    ids = re.findall(r'^PRIM\((\w+),', pathlib.Path("src/base/prims.def").read_text(), re.M)
    def save(name, id_name, text):
        assert len(text) <= 64
        raw = bytes([ids.index(id_name), 1, 0, 0, len(text)])
        raw += b"".join(struct.pack("<Q", ord(c) << 1) for c in text)
        (directory / name).write_bytes(raw)
    for number, row in rows(sources["auxiliary/GraphemeBreakTest.txt"]):
        text = "".join(chr(int(t, 16)) for t in row.split() if t not in {"÷", "×"})
        save(f"ucd-grapheme-{number}", "UNICODE16_GRAPHEMES", text)
        if number % 16 == 0:
            save(f"ucd-width-{number}", "UNICODE16_DISPLAY_WIDTH", text)
    index = 0
    for number, row in rows(sources["NormalizationTest.txt"]):
        if row.startswith("@"):
            continue
        if index < 80 or index % 64 == 0 or index > 19948:
            save(f"ucd-nfc-{number}", "UNICODE16_NORMALIZE_NFC", string(row.split(";")[0]))
        index += 1
    for number, row in rows(sources["SpecialCasing.txt"]):
        cp = chr(int(row.split(";")[0], 16))
        save(f"ucd-upper-{number}", "UNICODE16_UPPERCASE", "A" + cp + "Σ")
        save(f"ucd-lower-{number}", "UNICODE16_LOWERCASE", "AΣ" + cp)
    for i, base in enumerate("#*0123456789"):
        for j, selector in enumerate(["", "\ufe0f"]):
            save(f"ucd-keycap-{i}-{j}", "UNICODE16_DISPLAY_WIDTH", base + selector + "\u20e3")
    for i, text in enumerate(["\x00\t\n\r\n\x1f\x7f\x80\x9f", "🇺", "🇺🇸", "🇺🇸🇨", "🇺🇸🇨🇦",
                              "🇺\t🇸", "🇺\ufe0f", "🇺\u0308🇸"]):
        save(f"ucd-width-policy-{i}", "UNICODE16_DISPLAY_WIDTH", text)
    for i, text in enumerate(["0001-01-01T00:00:00Z", "9999-12-31T23:59:59.999999Z",
                              "2026-10-12T04:15:00.5+02:00", "1970-01-01T00:00:60Z"]):
        save(f"time-parse-{i}", "TIME_PARSE", text)


def main():
    if not sys.argv[1:]:
        raise AssertionError("at least one C driver is required")
    sources = read_sources()
    if len(sys.argv) == 3 and sys.argv[1] == "--corpus":
        corpus(sources, pathlib.Path(sys.argv[2]))
        return
    cases, description = fixtures(sources)
    data = "".join(raw for _, raw, _ in cases)
    expected_categories = categories(sources)

    def check(driver):
        classified = subprocess.run([driver, "--categories"], capture_output=True, timeout=180)
        if classified.returncode:
            raise AssertionError(f"{driver}: classification failed\n{classified.stderr!r}")
        if classified.stdout != expected_categories:
            if len(classified.stdout) != len(expected_categories):
                raise AssertionError(f"{driver}: classification result length differs")
            for index, (actual, want) in enumerate(zip(classified.stdout, expected_categories)):
                if actual != want:
                    cp = index if index < 0xD800 else index + 0x800
                    raise AssertionError(f"{driver}: U+{cp:04X} category bits {actual}, wanted {want}")
        result = subprocess.run([driver, "--oracle"], input=data, text=True,
                                capture_output=True, timeout=180)
        if result.returncode:
            raise AssertionError(f"{driver}: exit {result.returncode}\n{result.stderr}\n{result.stdout[-1000:]}")
        found = result.stdout.splitlines()
        if len(found) != len(cases):
            raise AssertionError(f"{driver}: {len(found)} results, expected {len(cases)}")
        for (label, raw, want), actual in zip(cases, found):
            if want != actual:
                raise AssertionError(f"{driver}: {label}\ninput: {raw}want: {want}\nfound: {actual}")
        start = time.monotonic()
        work = subprocess.run([driver, "--work"], text=True, capture_output=True, timeout=20)
        if work.returncode:
            raise AssertionError(f"{driver}: work failed\n{work.stdout}\n{work.stderr}")
        return (f"{driver}: letter/digit agree at all {len(expected_categories)} scalars; "
                f"{len(cases)} Unicode calls agree; work {time.monotonic() - start:.2f}s")

    print("Unicode 16: " + description)
    with concurrent.futures.ThreadPoolExecutor() as pool:
        for verdict in pool.map(check, sys.argv[1:]):
            print(verdict)


if __name__ == "__main__":
    main()
