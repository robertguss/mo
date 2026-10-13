#!/usr/bin/env python3
"""Generate Mo's Unicode 16 tables from hash-pinned, offline UCD sources.

Import the official UCD.zip with --archive PATH once; ordinary generation and
--check only use the checked-in compressed sources, never the network or the
host Python Unicode database.
"""
import argparse
import array
import functools
import gzip
import hashlib
import json
import pathlib
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
DATA = ROOT / "ci/unicode16"
TARGET = ROOT / "src/prim/unicode16-data.h"
ARCHIVE_SHA256 = "c86dd81f2b14a43b0cc064aa5f89aa7241386801e35c59c7984e579832634eb2"
SOURCES = ("UnicodeData.txt", "SpecialCasing.txt", "DerivedCoreProperties.txt",
           "DerivedNormalizationProps.txt", "EastAsianWidth.txt",
           "auxiliary/GraphemeBreakProperty.txt", "auxiliary/GraphemeBreakTest.txt",
           "NormalizationTest.txt", "emoji/emoji-data.txt", "emoji/emoji-variation-sequences.txt")
GCB = ("Other", "CR", "LF", "Control", "Extend", "ZWJ", "Regional_Indicator",
       "Prepend", "SpacingMark", "L", "V", "T", "LV", "LVT")
FLAGS = {"PICTOGRAPHIC": 1 << 6, "EMOJI": 1 << 7, "CASED": 1 << 8,
         "IGNORABLE_CASE": 1 << 9, "ZERO": 1 << 10, "WIDE": 1 << 11, "VARIANT": 1 << 12}


def source_path(name):
    return DATA / (name.replace("/", "-") + ".gz")


def import_archive(path):
    raw = pathlib.Path(path).read_bytes()
    if hashlib.sha256(raw).hexdigest() != ARCHIVE_SHA256:
        raise AssertionError("not the pinned Unicode 16.0.0 UCD.zip")
    DATA.mkdir(parents=True, exist_ok=True)
    manifest = {}
    with zipfile.ZipFile(path) as archive:
        for name in SOURCES:
            data = archive.read(name)
            source_path(name).write_bytes(gzip.compress(data, mtime=0))
            manifest[name] = hashlib.sha256(data).hexdigest()
    (DATA / "sha256.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")


def read_sources():
    manifest = json.loads((DATA / "sha256.json").read_text())
    if set(manifest) != set(SOURCES):
        raise AssertionError("Unicode source manifest differs")
    result = {}
    for name in SOURCES:
        data = gzip.decompress(source_path(name).read_bytes())
        if hashlib.sha256(data).hexdigest() != manifest[name]:
            raise AssertionError(f"Unicode source hash differs: {name}")
        result[name] = data.decode("utf-8")
    return result


def records(text):
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            yield [field.strip() for field in line.split(";")]


def interval(field):
    values = [int(n, 16) for n in field.split("..")]
    first, last = values[0], values[-1]
    assert 0 <= first <= last <= 0x10FFFF
    return range(first, last + 1)


def generate(sources):
    props = array.array("I", [0]) * 0x110000
    decompositions, lower, upper = {}, {}, {}
    first = None
    for row in records(sources["UnicodeData.txt"]):
        assert len(row) == 15
        c = int(row[0], 16)
        if row[1].endswith(", First>"):
            assert first is None
            first = c
            continue
        start = first if row[1].endswith(", Last>") else c
        if first is not None:
            assert row[1].endswith(", Last>")
        first = None
        for cp in range(start, c + 1):
            props[cp] |= int(row[3]) << 16
            if row[2].startswith("M"):
                props[cp] |= FLAGS["ZERO"]
        if row[5] and not row[5].startswith("<"):
            decompositions[c] = tuple(int(x, 16) for x in row[5].split())
        if row[12]:
            upper[c] = (int(row[12], 16),)
        if row[13]:
            lower[c] = (int(row[13], 16),)
    assert first is None
    for row in records(sources["SpecialCasing.txt"]):
        conditions = row[4].split()
        if conditions:
            assert conditions == ["Final_Sigma"] or any(x in conditions for x in ["tr", "az", "lt"])
            continue
        c = int(row[0], 16)
        lower[c] = tuple(int(x, 16) for x in row[1].split())
        upper[c] = tuple(int(x, 16) for x in row[3].split())
    for row in records(sources["auxiliary/GraphemeBreakProperty.txt"]):
        value = GCB.index(row[1])
        # Modern Hangul LV/LVT alternation is computed in the lookup, not stored.
        for c in interval(row[0]):
            if not 0xAC00 <= c <= 0xD7A3:
                assert props[c] & 15 == 0
                props[c] |= value
    core = {"Cased": FLAGS["CASED"], "Case_Ignorable": FLAGS["IGNORABLE_CASE"],
            "Default_Ignorable_Code_Point": FLAGS["ZERO"]}
    for row in records(sources["DerivedCoreProperties.txt"]):
        value = core.get(row[1], 0)
        if row[1] == "InCB":
            value = ("None", "Consonant", "Extend", "Linker").index(row[2]) << 4
        if value:
            for c in interval(row[0]):
                props[c] |= value
    for row in records(sources["EastAsianWidth.txt"]):
        assert row[1] in {"N", "Na", "A", "H", "W", "F"}
        if row[1] in {"W", "F"}:
            for c in interval(row[0]):
                props[c] |= FLAGS["WIDE"]
    for row in records(sources["emoji/emoji-data.txt"]):
        value = {"Extended_Pictographic": FLAGS["PICTOGRAPHIC"],
                 "Emoji_Presentation": FLAGS["EMOJI"]}.get(row[1], 0)
        for c in interval(row[0]):
            props[c] |= value
    for row in records(sources["emoji/emoji-variation-sequences.txt"]):
        c, selector = (int(x, 16) for x in row[0].split())
        assert selector in {0xFE0E, 0xFE0F}
        props[c] |= FLAGS["VARIANT"]
    excluded = set()
    for row in records(sources["DerivedNormalizationProps.txt"]):
        if row[1] == "Full_Composition_Exclusion":
            excluded.update(interval(row[0]))
    compositions = sorted((ds[0], ds[1], c) for c, ds in decompositions.items()
                          if len(ds) == 2 and c not in excluded)
    assert len({(a, b) for a, b, _ in compositions}) == len(compositions)

    @functools.cache
    def expand(c):
        return tuple(x for d in decompositions[c] for x in expand(d)) if c in decompositions else (c,)

    decompositions = {c: expand(c) for c in decompositions}
    assert max(map(len, decompositions.values())) <= 4
    assert max(map(len, lower.values())) <= 3 and max(map(len, upper.values())) <= 3
    ranges = []
    start = 0
    for c in range(1, len(props) + 1):
        if c == len(props) or props[c] != props[start]:
            if props[start]:
                ranges.append((start, c - 1, props[start]))
            start = c
    lines = ["/* Generated by ci/generate_unicode16.py from Unicode 16.0.0; do not edit.",
             " * Source hashes and Unicode license: ci/unicode16/. */",
             "#ifndef MO_UNICODE16_DATA_H", "#define MO_UNICODE16_DATA_H", "",
             "#include <stdint.h>", "",
             "/* clang-format off */", "enum mo_unicode_gcb {"]
    lines += [f"    MO_GCB_{name.upper()} = {i}," for i, name in enumerate(GCB)]
    lines += ["};", ""]
    lines += [f"#define MO_UCD_{name} {value}U" for name, value in FLAGS.items()]
    lines += ["", "struct mo_unicode_range { uint32_t first, last, properties; };",
              "struct mo_unicode_mapping { uint32_t scalar, offset, count; };",
              "struct mo_unicode_composition { uint32_t left, right, result; };", "",
              "static const struct mo_unicode_range mo_unicode_ranges[] = {"]
    lines += [f"    {{0x{a:x}, 0x{b:x}, 0x{p:x}}}," for a, b, p in ranges]
    lines += ["};", ""]
    lines.append("static const uint32_t mo_unicode_ascii[] = {")
    for i in range(0, 128, 8):
        lines.append("    " + ", ".join(f"0x{x:x}" for x in props[i:i+8]) + ",")
    lines += ["};", ""]
    pool, offsets = [], {}
    for name, table in [("decompose", decompositions), ("lower", lower), ("upper", upper)]:
        lines.append(f"static const struct mo_unicode_mapping mo_unicode_{name}[] = {{")
        for c, mapping in sorted(table.items()):
            assert mapping and all(0 <= x <= 0x10FFFF and not 0xD800 <= x <= 0xDFFF for x in mapping)
            if mapping == (c,):
                continue
            if mapping not in offsets:
                offsets[mapping] = len(pool)
                pool.extend(mapping)
            lines.append(f"    {{0x{c:x}, {offsets[mapping]}, {len(mapping)}}},")
        lines += ["};", ""]
    lines.append("static const uint32_t mo_unicode_scalars[] = {")
    for i in range(0, len(pool), 12):
        lines.append("    " + ", ".join(f"0x{c:x}" for c in pool[i:i+12]) + ",")
    lines += ["};", "", "static const struct mo_unicode_composition mo_unicode_compose[] = {"]
    lines += [f"    {{0x{a:x}, 0x{b:x}, 0x{c:x}}}," for a, b, c in compositions]
    lines += ["};", "/* clang-format on */", "", "#endif", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", help="import the hash-pinned official UCD.zip")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if args.archive:
        import_archive(args.archive)
    result = generate(read_sources())
    if args.check:
        if TARGET.read_text() != result:
            raise AssertionError("generated Unicode 16 tables differ; regenerate them")
    else:
        TARGET.write_text(result)
    print(f"Unicode 16: source hashes and generated tables agree ({len(result)} source bytes)")


if __name__ == "__main__":
    main()
