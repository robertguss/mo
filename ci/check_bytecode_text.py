#!/usr/bin/env python3
"""Bound repeated layout work and decimal BigInt conversion through the public assembler."""
import subprocess
import sys
import time

sys.set_int_max_str_digits(0)
integer = 10**4096 - 1
expected = "magnitude " + integer.to_bytes((integer.bit_length() + 7) // 8, "little").hex()

for driver in sys.argv[1:]:
    # Broad bands for debug and sanitizer builds, not a machine-speed benchmark.
    # Both old per-request layout walks and old per-digit conversion exceed these.
    ceiling = 20 if "/sanitize/" in driver else 10
    for kind in ("record", "case", "bigint"):
        start = time.monotonic()
        try:
            result = subprocess.run([driver, "--work", kind], text=True, capture_output=True,
                                    check=True, timeout=ceiling)
        except subprocess.TimeoutExpired:
            raise SystemExit(f"{driver}: {kind} exceeded {ceiling}s work ceiling")
        except subprocess.CalledProcessError as error:
            raise SystemExit(f"{driver}: {kind}: {error.stdout}{error.stderr}")
        lines = result.stdout.splitlines()
        if not lines or lines[-1] != "bytecode text work: all checks passed":
            raise SystemExit(f"{driver}: {kind}: missing successful result")
        if kind == "bigint" and lines[0] != expected:
            raise SystemExit(f"{driver}: decimal BigInt differs from exact Python integer")
        print(f"{driver}: {kind} work passed in {time.monotonic() - start:.2f}s "
              f"(ceiling {ceiling}s)", flush=True)
