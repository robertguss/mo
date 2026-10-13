#!/usr/bin/env python3
"""Heap text against the exact Lean Float oracle and Python integer arithmetic."""
import concurrent.futures
import random
import struct
import subprocess
import sys

sys.set_int_max_str_digits(0)
rng = random.Random(236)
bits = [0, 1, 2, 0x000FFFFFFFFFFFFF, 0x0010000000000000,
        0x7FEFFFFFFFFFFFFF, 0x7FF0000000000000, 0x7FF8000000000001]
for exponent in range(1, 2047, 31):
    center = exponent << 52
    bits.extend([center - 1, center, center + 1])
for exponent in [-324, -323, -308, -22, -7, -6, -5, -1, 0, 1, 20, 21, 22, 307, 308]:
    center = struct.unpack("Q", struct.pack("d", float(f"1e{exponent}")))[0]
    bits.extend([max(0, center - 1), center, center + 1])
bits = sorted(set(bits + [n | (1 << 63) for n in bits] +
                  [rng.getrandbits(64) for _ in range(2048)]))
float_input = "".join(f"{n}\n" for n in bits)

def run(command, data, cwd=None):
    result = subprocess.run(command, input=data, text=True, capture_output=True,
                            check=True, timeout=180, cwd=cwd)
    return result.stdout.splitlines()

expected = run(["lake", "env", "lean", "--run", "HeapFloatOracle.lean"], float_input, "formal")
if len(expected) != len(bits):
    raise SystemExit(f"Lean returned {len(expected)} lines for {len(bits)} inputs")

magnitudes = []
for size in [1, 2, 3, 4, 5, 31, 32, 33, 63, 64, 65, 255, 256, 257, 4095, 4096, 4097,
             32767, 65535, 65536, 65537, 262143, 262144]:
    magnitudes.extend([b"\xff" * size, b"\0" * (size - 1) + b"\x01"])
for _ in range(96):
    size = rng.randrange(1, 2049)
    magnitudes.append(rng.randbytes(size - 1) + bytes([rng.randrange(1, 256)]))
integer_input = "".join(m.hex() + "\n" for m in magnitudes)
integers = [str(int.from_bytes(m, "little")) for m in magnitudes]

def check(driver):
    actual = run([driver, "--float"], float_input)
    if actual != expected:
        for i, (a, e) in enumerate(zip(actual, expected)):
            if a != e:
                raise SystemExit(f"{driver}: Float bits {bits[i]:016x}: {a!r} != Lean {e!r}")
        raise SystemExit(f"{driver}: Float output count differs")
    if run([driver, "--bigint"], integer_input) != integers:
        raise SystemExit(f"{driver}: BigInt decimal output differs from Python")
    return f"{driver}: {len(bits)} Float spellings match Lean; {len(integers)} BigInts match Python"

with concurrent.futures.ThreadPoolExecutor() as pool:
    for verdict in pool.map(check, sys.argv[1:]):
        print(verdict)
