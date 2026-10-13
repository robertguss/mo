#!/usr/bin/env python3
"""Compare primitive results, Float bits and exact stop details with mocore."""
import concurrent.futures
import pathlib
import random
import re
import struct
import subprocess
import sys
import time

sys.set_int_max_str_digits(0)
VERSION = re.search(r'#define MO_CORE_TEXT_VERSION "([^"]+)"',
                    pathlib.Path("src/core/core.h").read_text())[1]


def quote(s):
    escapes = {'"': '\\"', "\\": "\\\\", "\n": "\\n", "\r": "\\r", "\t": "\\t"}
    return '"' + "".join(escapes.get(c, f"\\u{{{ord(c):x}}}" if ord(c) < 32 or ord(c) == 127 else c)
                         for c in s) + '"'


def camel(s):
    return re.sub(r"_([a-z])", lambda m: m[1].upper(), s)


def floating(n):
    return f"(float 0x{n:016x})"


def float_bits(n):
    return struct.unpack("<Q", struct.pack("<d", n))[0]


def query(name, primitive, args):
    return f"(core {VERSION} (tuple ((text {quote(name)}) (text {quote(primitive)}) {' '.join(args)})))\n"


def text_cases(rng, rows):
    texts = ["", "a", "aba", "ababa", "aaaaa", "aaaab", "a\n", "\n\n", "a\r\nb\r",
             " \t\n\r\v\f", " a  b \n c ", "\u00a0a\u00a0", "β🙂", "a\0b", "#{x}", '"\\\x7f']
    texts += ["".join(rng.choice("aβ🙂 \n\r\t\0") for _ in range(rng.randrange(33))) for _ in range(24)]
    number_texts = ["", "-", "+1", "00", "-0", "0x10", "1_000", " 1", "1 ", "1.", ".1",
                    "9223372036854775807", "9223372036854775808", "-9223372036854775808",
                    "-9223372036854775809", "170141183460469231731.687303715884105727",
                    "170141183460469231731.687303715884105728", "0.000000000000000001",
                    "0.0000000000000000001", "nan", "inf", "-inf", "NaN", "1e-324", "5e-324",
                    "1e309", "1e-9999", "-0.0", "1e+20", "0.100000000000000000000000000001"]
    number_texts += [str(rng.randrange(-(1 << 64), 1 << 64)) for _ in range(48)]
    for _, module, name, _, params in rows:
        if module not in {"Text", "Char", "Bytes"}:
            continue
        op = {"%concat": "concat", "empty?": "isEmpty", "starts_with?": "startsWith",
              "ends_with?": "endsWith", "contains?": "contains", "ascii_lowercase": "lower",
              "ascii_uppercase": "upper", "ascii_digit?": "isDigit", "ascii_letter?": "isLetter",
              "ascii_white_space?": "isWhiteSpace"}.get(name, camel(name))
        primitive = f"({module.lower()} ({op}))"
        values = []
        if name == "%compare":
            primitive = f"(order ({module.lower()}))"
        if name == "%quoted":
            primitive = "(quotedText)" if module == "Text" else "(quotedChar)"
        if module == "Text":
            if name == "%structural":
                primitive = "(structural (int))"
                values = [[f"(int {n})"] for n in [-(1 << 63), -1, 0, 1, (1 << 63)-1]]
            elif name == "from_chars":
                values = [["(list (" + " ".join(f"(char {quote(c)})" for c in s) + "))"] for s in texts]
            elif name in {"to_int", "to_decimal", "to_float"}:
                values = [[f"(text {quote(s)})"] for s in number_texts]
            elif name in {"repeat", "pad_left", "pad_right"}:
                values = [[f"(text {quote(s)})", f"(int {n})"] for s in texts for n in [-1, 0, 1, 2, 7]]
            elif name == "replace":
                values = [[f"(text {quote(s)})", f"(text {quote(p)})", f"(text {quote(r)})"]
                          for s in texts for p in ["", "a", "aa", "aba", "🙂"] for r in ["", "β", "ab"]]
            elif len(params) == 2:
                values = [[f"(text {quote(s)})", f"(text {quote(p)})"]
                          for s in texts for p in ["", "a", "aa", "aba", "🙂", "nomatch"]]
            else:
                values = [[f"(text {quote(s)})"] for s in texts]
        elif module == "Char":
            scalars = [0, 9, 10, 11, 12, 13, 32, 34, 39, 48, 57, 58, 65, 90, 91, 97, 122, 123,
                       127, 128, 0xA0, 0xD7FF, 0xE000, 0x1F642, 0x10FFFF]
            scalars += [n for n in [rng.randrange(0x110000) for _ in range(48)]
                        if not 0xD800 <= n <= 0xDFFF]
            values = [[f"(char {quote(chr(n))})"] for n in scalars]
        else:
            byte_sequences = [b"", b"\0", b"abc", b"\xff", b"\xc0\x80", b"\xed\xa0\x80",
                              b"\xf4\x8f\xbf\xbf", b"\xf4\x90\x80\x80", "β🙂".encode()]
            byte_sequences += [rng.randbytes(rng.randrange(33)) for _ in range(24)]
            enc = lambda b: "(bytes (" + " ".join(str(n) for n in b) + "))"
            if name == "get":
                values = [[enc(b), f"(int {n})"] for b in byte_sequences for n in [-1, 0, len(b)-1, len(b), (1 << 63)-1]]
            elif name == "%compare":
                values = [[enc(a), enc(b)] for a in byte_sequences for b in byte_sequences[:9]]
            else:
                values = [[enc(b)] for b in byte_sequences]
        for i, args in enumerate(values):
            yield f"{module}.{name}-{i}", query(f"{module}.{name}", primitive, args)


def big_cases(rng, rows):
    samples = [0, 1, -1, -(1 << 63), (1 << 63)-1, 1 << 63, -(1 << 63)-1,
               (1 << 32)-1, (1 << 64)-1, 1 << 64, (1 << 128)-1, -(1 << 128)]
    samples += [rng.getrandbits(bits) * rng.choice([-1, 1])
                for bits in [31, 32, 33, 63, 64, 65, 127, 128, 129, 511, 512, 513, 2048, 2049, 4096, 8192]
                for _ in range(3)]
    encode = lambda n: f"(bigInt {n})"
    for _, module, name, _, params in rows:
        if module != "BigInt":
            continue
        op = camel(name.lstrip("%"))
        primitive = f"(big ({op}))" if name != "%compare" else "(order (bigInt))"
        if name == "pow":
            bases = samples[:12] + [rng.randrange(-10000, 10001) for _ in range(16)]
            args = [[encode(a), f"(int {n})"] for a in bases for n in [-1, 0, 1, 2, 3, 7, 32]]
        elif len(params) == 2:
            args = [[encode(a), encode(b)] for a in samples for b in samples]
            # Same leading limbs but a smaller low limb forces the division
            # estimator's add-back path, which uniform random limbs rarely hit.
            divisors = [(high << 64) + low for high in [1 << 31, (1 << 32)-1]
                        for low in [1, (1 << 32)-1]]
            args += [[encode((b * (1 << 32) - 1) * sign), encode(b * other)]
                     for b in divisors for sign in [-1, 1] for other in [-1, 1]]
        else:
            args = [[encode(a)] for a in samples]
        for i, values in enumerate(args):
            yield f"BigInt.{name}-{i}", query(f"BigInt.{name}", primitive, values)


def collection_cases(rng, rows):
    def seq(kind, values):
        return f"({kind} ({' '.join(values)}))"
    enc_int = lambda n: f"(int {n})"
    enc_text = lambda s: f"(text {quote(s)})"
    lists = [[], [1], [3, 1, 3, -2, 1, 4], [-(1 << 63), -1], [(1 << 63)-1, 1, -1]]
    lists += [[rng.randrange(-10, 11) for _ in range(rng.randrange(32))] for _ in range(24)]
    text_lists = [[], [""], ["a", "β", "", "a"], ["🙂", "x\0y"]]
    text_lists += [["".join(rng.choice("aβ🙂\0") for _ in range(rng.randrange(8)))
                    for _ in range(rng.randrange(8))] for _ in range(16)]
    int_lists = [seq("list", list(map(enc_int, xs))) for xs in lists]
    sets = [seq("set", list(map(enc_int, dict.fromkeys(xs)))) for xs in lists]
    pairs = [[], [(3, "a"), (1, "β"), (3, "last")], [(-1, ""), (5, "🙂")]]
    pairs += [[(rng.randrange(-4, 5), "".join(rng.choice("abβ🙂") for _ in range(rng.randrange(8))))
               for _ in range(rng.randrange(16))] for _ in range(24)]
    entries = lambda xs: [seq("tuple", [enc_int(k), enc_text(v)]) for k, v in xs]
    maps = ["(map (" + " ".join(f"({enc_int(k)} {enc_text(v)})" for k, v in xs) + "))"
            for xs in [[], [(3, "a"), (1, "β")], [(-1, ""), (5, "🙂")]] +
            [list(dict(ps).items()) for ps in pairs[3:]]]
    for _, module, name, _, params in rows:
        if module not in {"List", "Map", "Set", "Range"}:
            continue
        op = {"%empty": "lit 0", "empty?": "isEmpty", "contains?": "contains", "has?": "has"}.get(name, camel(name))
        primitive = {"List": f"(list (int) ({op}))", "Map": f"(map (int) (text) ({op}))",
                     "Set": f"(set (int) ({op}))", "Range": f"(range ({op}))"}[module]
        values = []
        if module == "List":
            if name == "to_set":
                primitive = "(set (int) (ofList))"
            elif name == "to_map":
                primitive = "(map (int) (text) (ofList))"
                values = [[seq("list", entries(xs))] for xs in pairs]
            elif name == "join":
                primitive = "(text (join))"
                values = [[seq("list", list(map(enc_text, xs))), enc_text(s)]
                          for xs in text_lists for s in ["", "-", "🙂"]]
            elif name == "sum":
                primitive = "(sum (int))"
            elif name == "zip":
                primitive = "(list (int) (zip (text)))"
                values = [[xs, seq("list", list(map(enc_text, ys)))] for xs in int_lists for ys in text_lists]
            if not values:
                if not params:
                    values = [[]]
                elif name in {"append", "prepend", "contains?"}:
                    values = [[xs, enc_int(n)] for xs in int_lists for n in [-1, 0, 1, 3, 10]]
                elif name == "concat":
                    values = [[xs, ys] for xs in int_lists for ys in int_lists[:3]]
                elif name in {"get", "take", "drop", "set", "remove_at"}:
                    values = [[xs, enc_int(n)] + ([enc_int(99)] if name == "set" else [])
                              for raw, xs in zip(lists, int_lists) for n in [-1, 0, 1, len(raw)-1, len(raw), (1 << 63)-1]]
                else:
                    values = [[xs] for xs in int_lists]
        elif module == "Map":
            if name == "%literal_insert":
                primitive = "(literalInsert)"
            if not params:
                values = [[]]
            elif len(params) == 1:
                values = [[m] for m in maps]
            else:
                values = [[m, enc_int(n)] + ([enc_text("new")] if len(params) == 3 else [])
                          for m in maps for n in [-1, 0, 1, 3, 5, 10]]
        elif module == "Set":
            if not params:
                values = [[]]
            elif len(params) == 1:
                values = [[s] for s in sets]
            elif name in {"union", "intersection", "difference"}:
                values = [[s, t] for s in sets for t in sets[:4]]
            else:
                values = [[s, enc_int(n)] for s in sets for n in [-1, 0, 1, 3, 10]]
        else:
            ranges = [(0, 0), (3, 1), (-3, 4), ((1 << 63)-3, (1 << 63)-1), (-(1 << 63), -(1 << 63)+3)]
            ranges += [(a, a + rng.randrange(-5, 17)) for a in
                       [rng.randrange(-(1 << 63) + 32, (1 << 63) - 32) for _ in range(24)]]
            if name == "to_list":
                values = [[f"(range {a} {b})"] for a, b in ranges]
            else:
                values = [[f"(range {a} {b})", enc_int(n)] for a, b in ranges for n in [a-1, a, b-1, b]
                          if -(1 << 63) <= n < (1 << 63)]
        for i, args in enumerate(values):
            yield f"{module}.{name}-{i}", query(f"{module}.{name}", primitive, args)


def time_cases(rng):
    low, high = -62135596800000000, 253402300799999999
    times = [low, low + 1, -86400000001, -1, 0, 1, high - 1, high]
    times += [rng.randrange(low, high + 1) for _ in range(128)]
    durations = [-(1 << 63), (1 << 63)-1, -1001, -999, -1, 0, 1, 999, 1001]
    durations += [rng.randrange(-(1 << 63), 1 << 63) for _ in range(128)]
    for name, op in [("in_milliseconds", "inMilliseconds"), ("in_seconds", "inSeconds"),
                     ("to_text", "durToText")]:
        for i, d in enumerate(durations):
            yield f"Duration.{name}-{i}", query(f"Duration.{name}", f"(time ({op}))", [f"(duration {d})"])
    for i, t in enumerate(times):
        yield f"Time.to_text-{i}", query("Time.to_text", "(time (toText))", [f"(time {t})"])
        for d in durations[:9] + [low - t - 1, low - t, high - t, high - t + 1]:
            yield f"Time.checked_add-{i}-{d}", query("Time.checked_add", "(time (checkedAdd))",
                                                    [f"(time {t})", f"(duration {d})"])
    texts = ["", "0001-01-01T00:00:00Z", "9999-12-31T23:59:59.999999Z",
             "0001-01-01T00:00:00+00:01", "9999-12-31T23:59:59-00:01",
             "0000-12-31T23:59:59-00:01", "0000-02-29T00:00:00Z",
             "2000-02-29t23:59:59.123456z", "1900-02-29T00:00:00Z",
             "2026-10-12T04:15:00.5+02:00", "1970-01-01T00:00:00-23:59",
             "1970-01-01T00:00:00+24:00", "1970-01-01T00:00:00+01:60",
             "1970-01-01T00:00:60Z", "1970-01-01T24:00:00Z",
             "1970-01-01T00:00:00.Z", "1970-01-01T00:00:00.1234567Z",
             "1970-00-01T00:00:00Z", "1970-13-01T00:00:00Z",
             "1970-01-00T00:00:00Z", "1970-04-31T00:00:00Z",
             "1970-01-01 00:00:00Z", "1970-01-01T00:00:00Z\0"]
    for _ in range(256):
        y, m, d = rng.randrange(10000), rng.randrange(14), rng.randrange(33)
        hh, mm, ss = rng.randrange(26), rng.randrange(62), rng.randrange(62)
        fraction = rng.choice(["", ".", f".{rng.randrange(10**7):07}", f".{rng.randrange(10**6):06}"])
        zone = rng.choice(["Z", "z", f"+{rng.randrange(26):02}:{rng.randrange(62):02}", "-23:59"])
        texts.append(f"{y:04}-{m:02}-{d:02}T{hh:02}:{mm:02}:{ss:02}{fraction}{zone}")
    # Mutations hit every fixed-width digit and separator, not only range checks.
    canonical = "2000-02-29T12:34:56.123456+02:30"
    texts += [canonical[:i] + c + canonical[i+1:] for i in range(len(canonical)) for c in ["x", "0", "9"]]
    for i, s in enumerate(texts):
        yield f"Time.parse-{i}", query("Time.parse", "(time (parse))", [f"(text {quote(s)})"])


def cases():
    rng = random.Random(1300)
    rows = re.findall(r'^PRIM\((\w+),\s*"([^"]+)",\s*"([^"]+)",\s*"([^"]+)",\s*"([ob]*)",',
                      pathlib.Path("src/base/prims.def").read_text(), re.M)
    ints = [-(1 << 63), -(1 << 63) + 1, -257, -1, 0, 1, 255, 256, (1 << 53) + 1,
            (1 << 63) - 2, (1 << 63) - 1]
    scale, maximum = 10**18, (1 << 127) - 1
    decimals = [-maximum, -maximum + 1, -scale - 1, -scale // 2, -5 * 10**16,
                -1, 0, 1, 5 * 10**16, scale // 2, scale + 1, maximum - 1, maximum]
    float_samples = [0, 1, 0x000FFFFFFFFFFFFF, 0x0010000000000000, 0x7FEFFFFFFFFFFFFF,
                     0x7FF0000000000000, 0x7FF8000000000001]
    float_samples += [float_bits(x) for x in [0.1, 0.5, 1.5, 2.5, 5e-19, 1.5e-18, 2.5e-18,
                                             1e-6, 1e20, 1.7014118346046923e20]]
    float_samples += [x | (1 << 63) for x in float_samples] + [rng.getrandbits(64) for _ in range(64)]
    for ident, module, name, signature, params in rows:
        if module not in {"Int", "Byte", "Decimal", "Float"}:
            continue
        family = {"Int": "int", "Byte": "byte", "Decimal": "dec", "Float": "float"}[module]
        op = {"nan?": "isNan", "infinite?": "isInfinite"}.get(name, camel(name))
        primitive = f"({family} ({op}))"
        samples = []
        if module == "Int":
            if name == "pow":
                samples = [(a, b) for a in [-2, -1, 0, 1, 2, -(1 << 63)]
                           for b in [-1, 0, 1, 2, 62, 63, 64]]
            elif len(params) == 2:
                samples = [(a, b) for a in ints for b in ints]
                samples += [(rng.randrange(-(1 << 63), 1 << 63), rng.randrange(-(1 << 63), 1 << 63))
                            for _ in range(48)]
            else:
                values = ints + [rng.randrange(-(1 << 63), 1 << 63) for _ in range(48)]
                if name == "to_char":
                    values += [0xD7FF, 0xD800, 0xDFFF, 0xE000, 0x10FFFF, 0x110000]
                multiplier = {"seconds": 10**6, "milliseconds": 1000, "minutes": 60 * 10**6,
                              "hours": 3600 * 10**6, "days": 86400 * 10**6}.get(name)
                if multiplier:
                    boundary = ((1 << 63) - 1) // multiplier
                    values += [boundary - 1, boundary, boundary + 1, -boundary - 1, -boundary]
                samples = [(a,) for a in values]
            values = [[f"(int {n})" for n in sample] for sample in samples]
        elif module == "Byte":
            values = [[f"(byte {n})"] for n in [0, 1, 127, 128, 254, 255] +
                      [rng.randrange(256) for _ in range(24)]]
        elif module == "Decimal":
            if name in {"round", "to_fixed"}:
                samples = [(a, p) for a in decimals for p in [-1, 0, 1, 17, 18, 19]]
                samples += [(rng.randrange(-maximum, maximum + 1), rng.randrange(19)) for _ in range(48)]
                values = [[f"(decimal {a})", f"(int {p})"] for a, p in samples]
            elif len(params) == 2:
                samples = [(a, b) for a in decimals for b in decimals]
                values = [[f"(decimal {a})", f"(decimal {b})"] for a, b in samples]
            else:
                samples = decimals + [rng.randrange(-maximum, maximum + 1) for _ in range(96)]
                values = [[f"(decimal {a})"] for a in samples]
        else:
            values = [[floating(n)] for n in float_samples]
        for i, args in enumerate(values):
            raw = query(f"{module}.{name}", primitive, args)
            yield f"{module}.{name}-{i}", raw
    yield from text_cases(rng, rows)
    yield from big_cases(rng, rows)
    yield from collection_cases(rng, rows)
    yield from time_cases(rng)
    for family, samples in [("decimal", [[], [-maximum, -1], [maximum, 1, -1], [-1, 1], [1, 2, 3]]),
                            ("bigInt", [[], [-(1 << 512), 1 << 511, (1 << 64)-1], [1, -1, 2]]),
                            ("float", [[], [1e30, -1e30, 0.1], [-0.0], [float('inf'), float('-inf')]])]:
        samples += [[rng.randrange(-100000, 100001) for _ in range(rng.randrange(24))] for _ in range(24)]
        for i, xs in enumerate(samples):
            values = [floating(float_bits(float(n))) if family == "float" else f"({family} {n})" for n in xs]
            yield f"List.sum-{family}-{i}", query("List.sum", f"(sum ({family}))", [f"(list ({' '.join(values)}))"])


def run(command, data):
    result = subprocess.run(command, input=data, text=True, capture_output=True, timeout=180)
    if result.returncode:
        raise AssertionError(f"{command}: exit {result.returncode}\n{result.stderr}\n{result.stdout[-1000:]}")
    return result.stdout.splitlines()


def extended_cases():
    rng = random.Random(1301)
    texts = ["", "β", "🙂", "#{x}", '"\\\0', "e\u0301"]
    enc = lambda s: f"(text {quote(s)})"
    seq = lambda tag, xs: f"({tag} ({' '.join(xs)}))"
    for size in [0, 1, 4, 8, 16, 64]:
        xs = [rng.choice(texts) for _ in range(size)]
        initial = seq("list", list(map(enc, xs)))
        for name, op, args in [("append", "append", [enc("β🙂")]),
                               ("contains?", "contains", [enc("β")]),
                               ("unique", "unique", []), ("to_set", "ofList", [])]:
            prim = f"(list (text) ({op}))" if name != "to_set" else "(set (text) (ofList))"
            yield f"List.{name}-utf8-{size}", query(f"List.{name}", prim, [initial] + args)
        initial = seq("set", list(map(enc, dict.fromkeys(xs))))
        for name, op, args in [("add", "add", [enc("🙂")]), ("remove", "remove", [enc("β")]),
                               ("has?", "has", [enc("#{x}")]), ("to_list", "toList", [])]:
            yield f"Set.{name}-utf8-{size}", query(f"Set.{name}", f"(set (text) ({op}))", [initial] + args)

    # Prefix queries compare every intermediate state after a long unique
    # update sequence, including tombstones, compaction and reinsert ordering.
    for family, initial in [("List", "(list ())"), ("Map", "(map ())"), ("Set", "(set ())")]:
        steps = []
        for i in range(256):
            key = f"(int {i % 16})"
            if family == "List":
                name, prim, args = "append", "(list (int) (append))", [key]
                if i >= 32 and i % 3 == 0:
                    name, prim, args = "set", "(list (int) (set))", [key, f"(int {-i})"]
            elif family == "Map":
                name = "remove" if i >= 16 and i % 3 == 0 else "set"
                prim = f"(map (int) (text) ({name}))"
                args = [key] + ([] if name == "remove" else [enc(texts[i % len(texts)])])
            else:
                name = "remove" if i >= 16 and i % 3 == 0 else "add"
                prim, args = f"(set (int) ({name}))", [key]
            steps.append(seq("tuple", [enc(f"{family}.{name}"), enc(prim)] + args))
            if i < 20 or i % 8 == 7:
                yield f"{family}-sequence-{i}", query(f"{family}.{'append' if family == 'List' else 'set' if family == 'Map' else 'add'}",
                                                     "(sequence)", [initial, seq("list", steps)])

    scalars = [("unit", "(unit)"), ("bool", "(bool true)"), ("byte", "(byte 255)"),
               ("char", f"(char {quote(chr(0x1f642))})"), ("text", enc("#{x}β")),
               ("bytes", "(bytes (0 255))"), ("decimal", "(decimal -1500000000000000000)"),
               ("bigInt", f"(bigInt {-(1 << 2049)})"), ("range", "(range -7 2)")]
    for tag, value in scalars:
        yield f"structural-{tag}", query("Text.%structural", f"(structural ({tag}))", [value])
    for bits in [float_bits(1.5), float_bits(-0.0), 0x7ff0000000000000, 0x7ff8000000000001]:
        yield f"structural-float-{bits}", query("Text.%structural", "(structural (float))", [floating(bits)])
    for tag, values in [("text", [enc(s) for s in texts]),
                        ("float", [floating(float_bits(n)) for n in [1.5, -0.0, 0.1]]),
                        ("bigInt", [f"(bigInt {1 << 2049})", "(bigInt -3)"])]:
        yield f"structural-list-{tag}", query("Text.%structural", f"(structural (list ({tag})))", [seq("list", values)])
    yield "structural-map", query("Text.%structural", "(structural (map (int) (text)))",
                                  [f"(map (((int -1) {enc('#{x}')}) ((int 3) {enc('β')})))"])


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--corpus":
        directory = pathlib.Path(sys.argv[2])
        directory.mkdir(parents=True, exist_ok=True)
        ids = re.findall(r'^PRIM\((\w+),', pathlib.Path("src/base/prims.def").read_text(), re.M)
        rng = random.Random(1300)
        for i, name in enumerate(ids[:ids.index("RANGE_CONTAINS") + 1]):
            for variant in range(6):
                operands = bytes(512) if variant == 0 else rng.randbytes(512)
                (directory / f"{name}-{variant}").write_bytes(
                    bytes([i, variant & 1, variant // 3, variant % 3]) + operands)
        return
    generated = list(cases()) + list(extended_cases())
    rows = re.findall(r'^PRIM\(\w+,\s*"([^"]+)",\s*"([^"]+)"',
                      pathlib.Path("src/base/prims.def").read_text(), re.M)
    expected_rows = {f"{module}.{name}" for module, name in rows
                     if module != "Unicode16"}
    covered = {re.search(r'\(text "([^"]+)"\)', raw)[1] for _, raw in generated}
    if covered != expected_rows:
        raise AssertionError(f"primitive oracle coverage differs: {covered ^ expected_rows}")
    data = "".join(raw for _, raw in generated)
    expected = run(["formal/.lake/build/bin/mocore", "--primitives"], data)
    if len(expected) != len(generated):
        raise AssertionError("mocore returned the wrong number of primitive results")

    def check(driver):
        for mode in ["--oracle", "--oracle-unique"]:
            actual = run([driver, mode], data)
            if len(actual) != len(expected):
                raise AssertionError(f"{driver}: wrong output count")
            for (label, raw), want, found in zip(generated, expected, actual):
                if want != found:
                    raise AssertionError(f"{driver} {mode}: {label}:\nLean: {want}\nC:    {found}\n{raw}")
        start = time.monotonic()
        work = subprocess.run([driver, "--work"], text=True, capture_output=True, timeout=20)
        if work.returncode:
            raise AssertionError(f"{driver}: work regression failed\n{work.stdout}\n{work.stderr}")
        elapsed = time.monotonic() - start
        # Above the old 256 KiB conversion cap. Check all emitted digits using
        # three independent modular fingerprints of 256^n - 1, not C's result.
        large = subprocess.run([driver, "--big-text-1m"], capture_output=True, text=True, timeout=120)
        if large.returncode or len(large.stdout) != 1000000 or not large.stdout.isdecimal():
            raise AssertionError(f"{driver}: million-digit conversion failed: {large.stderr}")
        for prime in [1000000007, 1000000009, 2147483647]:
            remainder = 0
            for i in range(0, len(large.stdout), 1000):
                chunk = large.stdout[i:i+1000]
                remainder = (remainder * pow(10, len(chunk), prime) + int(chunk)) % prime
            if remainder != (pow(256, 415241, prime) - 1) % prime:
                raise AssertionError(f"{driver}: million-digit fingerprint differs")
        return f"{driver}: {len(generated)} primitive calls match mocore (shared and unique); work regressions {elapsed:.2f}s"

    if not sys.argv[1:]:
        raise AssertionError("at least one C driver is required")
    with concurrent.futures.ThreadPoolExecutor() as pool:
        for result in pool.map(check, sys.argv[1:]):
            print(result)


if __name__ == "__main__":
    main()
