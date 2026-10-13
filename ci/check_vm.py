#!/usr/bin/env python3
"""Execute supported compiler output and compare outcomes with the Lean model.

The 15 original Core fixtures currently refuse in codegen; count them honestly.
Generated zero-argument wrappers exercise all currently supported forms through
parse, check, compile, write, load, initialize and call, under each C toolchain.
"""

import pathlib
import random
import subprocess
import sys
import tempfile
from fractions import Fraction

from check_codegen import expression
from check_core_check import VERSION, WORLD, parse, wire


def run(driver, path):
    result = subprocess.run([driver, "--core", str(path)], capture_output=True, text=True, timeout=15)
    if result.returncode == 77:
        return None
    assert result.returncode == 0, (driver, path, result.stderr)
    return result.stdout.rstrip("\n").split("\n", 1)


def oracle(path, world):
    result = subprocess.run(["formal/.lake/build/bin/mocore", str(path), str(world), "--fuel", "10000"],
                            capture_output=True, text=True, timeout=15)
    assert result.returncode in (0, 1, 2), (path, result.stdout, result.stderr)
    report = parse(result.stdout)[2]
    detail = report[1]
    if report[0] == "stopped":
        detail = detail[1] if detail[0] == "stop" else {"divByZero": "dividing by zero", "overflow": "overflow"}[detail[0]]
    return [str(report[0]), detail]


def decimals(drivers):
    maximum, scale = 2**127 - 1, 10**18
    edges = [-maximum, -maximum + 1, -2 * scale - 1, -scale, -3, -1, 0,
             1, 3, scale // 2, scale, scale + 1, maximum - 1, maximum]
    pairs = [(a, b) for a in edges for b in edges]
    rng = random.Random(207)
    pairs += [(rng.randrange(-maximum, maximum), rng.choice([
        rng.randrange(-maximum, maximum), rng.randrange(-2 * scale, 2 * scale),
        rng.randrange(-10, 10)])) for _ in range(250)]
    inputs, expected = [], []
    for a, b in pairs:
        for family in range(5):
            inputs.append(f"{family} {a} {b}\n")
            if family >= 3 and b == 0:
                expected.append("stopped")
                continue
            if family == 0: value = a + b
            elif family == 1: value = a - b
            elif family == 2: value = round(Fraction(a * b, scale))
            elif family == 3: value = round(Fraction(a * scale, b))
            else:
                quotient = abs(a) // abs(b) * (-1 if (a < 0) != (b < 0) else 1)
                value = a - quotient * b
            expected.append("stopped" if abs(value) > maximum else f"{value % 2**128:032x}")
    for driver in drivers:
        result = subprocess.run([driver, "--decimal"], input="".join(inputs),
                                text=True, capture_output=True, timeout=60)
        assert result.returncode == 0, (driver, result.stderr)
        got = result.stdout.splitlines()
        assert len(got) == len(expected)
        for index, (actual, want) in enumerate(zip(got, expected)):
            assert actual == want, (driver, inputs[index], want, actual)
        print(f"vm Decimal: {driver}: {len(expected)} results match exact Python rational arithmetic")


def main():
    drivers = sys.argv[1:]
    assert drivers, "built test_vm drivers are required"
    decimals(drivers)
    with tempfile.TemporaryDirectory(prefix="mo-vm-") as directory:
        root = pathlib.Path(directory)
        world = root / "world.core"
        world.write_text(f"(core {VERSION} {WORLD})\n")
        fixtures = []
        for path in sorted(pathlib.Path("tests/core").glob("*.core")):
            payload = parse(path.read_text())[2]
            program = payload[1] if payload[0] == "Run" else payload
            target = root / path.name
            target.write_text(f"(core {VERSION} {wire(program)})\n")
            fixtures.append(target)
        generated = []
        for i in range(256):
            body = expression(random.Random(207 + i), i % 4)
            flag = "true" if i % 2 else "false"
            program = f'''(Program () (
              (FunDecl "entry" () (int) (call "choose" ((lit (bool {flag})) (lit (int {i * 37 - 1000})))) "Generated" (function) 0)
              (FunDecl "choose" (("b" (bool)) ("x" (int))) (int) {body} "Generated" (function) 0)
              (FunDecl "identity" (("x" (int))) (int) (var "x") "Generated" (function) 0)
            ) () "entry" (noSource))'''
            path = root / f"generated-{i}.core"
            path.write_text(f"(core {VERSION} {program})\n")
            generated.append(path)
        for i, (ty, literal) in enumerate([
                ("unit", "unit"), ("bool", "bool true"), ("byte", "byte 255"),
                ("int", "int -9223372036854775808"), ("float", "float 0x8000000000000000"),
                ("char", 'char "🙂"'), ("time", "time -1"), ("duration", "duration -1"),
                ("decimal", "decimal -170141183460469231731687303715884105727")]):
            path = root / f"literal-{i}.core"
            path.write_text(f'(core {VERSION} (Program () ((FunDecl "entry" () ({ty}) (lit ({literal})) "M" (function) 0)) () "entry" (noSource)))\n')
            generated.append(path)
        expectations = {path: oracle(path, world) for path in generated}
        for driver in drivers:
            matched = unsupported = 0
            for path in fixtures + generated:
                got = run(driver, path)
                if got is None:
                    assert path in fixtures, ("supported generated program refused", path)
                    unsupported += 1
                    continue
                expected = expectations.get(path) or oracle(path, world)
                assert got == expected, (driver, path.read_text(), expected, got)
                matched += 1
            print(f"vm differential: {driver}: {matched} outcomes match mocore; {unsupported} fixtures explicitly unsupported by codegen")


if __name__ == "__main__":
    main()
