#!/usr/bin/env python3
"""Compare primitive results, Float bits and exact stop details with mocore."""
import concurrent.futures
import pathlib
import random
import re
import struct
import subprocess
import sys


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
            values = [[f"(byte {n})"] for n in [0, 1, 127, 128, 254, 255]]
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
            raw = f"(core 0.6 (tuple ((text {quote(module + '.' + name)}) (text {quote(primitive)}) {' '.join(args)})))\n"
            yield f"{module}.{name}-{i}", raw


def run(command, data):
    result = subprocess.run(command, input=data, text=True, capture_output=True, timeout=180)
    if result.returncode:
        raise AssertionError(f"{command}: exit {result.returncode}\n{result.stderr}\n{result.stdout[-1000:]}")
    return result.stdout.splitlines()


def main():
    generated = list(cases())
    data = "".join(raw for _, raw in generated)
    expected = run(["formal/.lake/build/bin/mocore", "--primitives"], data)
    if len(expected) != len(generated):
        raise AssertionError("mocore returned the wrong number of primitive results")

    def check(driver):
        actual = run([driver, "--oracle"], data)
        if len(actual) != len(expected):
            raise AssertionError(f"{driver}: wrong output count")
        for (label, raw), want, found in zip(generated, expected, actual):
            if want != found:
                raise AssertionError(f"{driver}: {label}:\nLean: {want}\nC:    {found}\n{raw}")
        return f"{driver}: {len(generated)} primitive calls match mocore"

    if not sys.argv[1:]:
        raise AssertionError("at least one C driver is required")
    with concurrent.futures.ThreadPoolExecutor() as pool:
        for result in pool.map(check, sys.argv[1:]):
            print(result)


if __name__ == "__main__":
    main()
