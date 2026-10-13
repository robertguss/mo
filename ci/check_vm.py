#!/usr/bin/env python3
"""Execute compiler output and compare outcomes with the Lean model.

Count compiler refusals and unavailable primitive/host dispatch separately,
never as matched outcomes. Pin the staging counts until those slices integrate.
Zero-argument wrappers and test selections run through parse, check, compile,
write, load, initialize and call, under each C toolchain.
"""

import pathlib
import random
import concurrent.futures
import itertools
import math
import struct
import subprocess
import sys
import tempfile
from fractions import Fraction

from check_codegen import closure_program, expression, global_program, reference_tail
from check_core_check import VERSION, WORLD, parse, wire, program

NEEDS_DISPATCH = {"needs primitive dispatch": 0, "needs host dispatch": 1}
CODEGEN_UNSUPPORTED = 15


def run(driver, path, test=None):
    args = [driver, "--core", str(path)] if test is None else [driver, "--core-test", str(path), test]
    result = subprocess.run(args, capture_output=True, text=True, timeout=15)
    if result.returncode == 77:
        return None
    assert result.returncode == 0, (driver, path, result.stderr)
    return result.stdout.rstrip("\n").split("\n", 1)


def oracle(path, world, test=None):
    args = ["formal/.lake/build/bin/mocore", str(path), str(world), "--fuel", "10000"]
    if test is not None:
        args += ["--test", test]
    result = subprocess.run(args,
                            capture_output=True, text=True, timeout=15)
    assert result.returncode in (0, 1, 2), (path, result.stdout, result.stderr)
    report = parse(result.stdout)[2]
    detail = report[1]
    if report[0] == "stopped":
        if detail[0] == "stop":
            detail = detail[1]
        elif detail[0] == "overflow":
            detail = ("time outside years 1 to 9999" if detail[1] == "a time outside the years 1 to 9999"
                      else f"overflow: {detail[1]}")
        else:
            detail = {"divByZero": "dividing by zero"}[detail[0]]
    # NaN sign/payload are not observable in Mo; C fmod and Lean need not
    # choose the same bits. Preserve every other bit, including signed zero.
    if report[0] == "returned" and isinstance(detail, str) and detail.startswith("0x"):
        if math.isnan(struct.unpack(">d", bytes.fromhex(detail[2:]))[0]):
            detail = "0x7ff8000000000000"
    return [str(report[0]), detail]


def operations(drivers, root, world):
    """One Lean primitive versus one verified bytecode instruction, not codegen.

    Both fuels are nonbinding here; their different units are never compared.
    Choice order compares declaration ordinals; cmp_float expands the Lean
    IEEE predicates and isNan into Ordering, including NaN greater than inf.
    """
    kinds = {"unit": 0, "bool": 1, "byte": 2, "int": 3, "float": 4,
             "char": 5, "time": 6, "duration": 7, "decimal": 8,
             "range": 9, "ordering": 13}
    maximum = 2**127 - 1
    edges = {
        "int": [-2**63, -2**63 + 1, -13, -1, 0, 1, 5, 2**63 - 2, 2**63 - 1],
        "decimal": [-maximum, -maximum + 1, -2 * 10**18 - 1, -10**18, -3, -1, 0,
                    1, 3, 10**18 // 2, 10**18, 10**18 + 1, maximum - 1, maximum],
        "float": [0, 0x8000000000000000, 0x3ff0000000000000, 0xbff0000000000000,
                  0x7ff0000000000000, 0xfff0000000000000, 0x7ff8000000000000,
                  0xfff8000000000001, 1, 0x7fefffffffffffff, 0x402b000000000000],
        "duration": [-2**63, -2**63 + 1, -1000001, -1, 0, 1, 999999, 2**63 - 1],
        "time": [-62135596800000000, -62135596799999999, -1, 0, 1,
                 253402300799999998, 253402300799999999],
        "bool": [0, 1], "byte": [0, 1, 128, 255],
        "char": [0, 0x7f, 0x80, 0xe9, 0xffff, 0x1f642, 0x10ffff],
        "ordering": [0, 1, 2], "range": [(-2**63, 2**63 - 1), (7, -9), (0, 0)],
    }

    def literal(kind, value):
        if kind == "float": return f"(lit (float 0x{value:016x}))"
        if kind == "bool": return f"(lit (bool {'true' if value else 'false'}))"
        if kind == "char": return f'(lit (char "\\u{{{value:x}}}"))'
        if kind == "ordering": return f'(con (ordering) "{["Less", "Equal", "Greater"][value]}" ())'
        if kind == "range": return f"(prim (range (make)) ((lit (int {value[0]})) (lit (int {value[1]}))))"
        return f"(lit ({kind} {value}))"

    def cell(kind, value):
        if kind == "range":
            return b"".join((n % 2**64).to_bytes(8, "little") for n in value).hex()
        width = 16 if kind == "decimal" else 8
        return ((value % 2**(8 * width)).to_bytes(width, "little") + bytes(16 - width)).hex()

    paths, inputs = [], []

    def add(op, a, b, ret, primitive, unary=False, body=None):
        for x, y in itertools.product(edges[a], [0] if unary else edges[b]):
            args = literal(a, x) + ("" if unary else " " + literal(b, y))
            expr = body(x, y) if body else f"(prim {primitive} ({args}))"
            path = root / f"operation-{len(paths)}.core"
            path.write_text(f"(core {VERSION} {program(expr, f'({ret})')})\n")
            paths.append(path)
            inputs.append(f"{op} {kinds[a]} {kinds[b]} {kinds[ret]} {cell(a, x)} {cell(b, y)}\n")

    for family, name in enumerate(["add", "sub", "mul", "div", "rem"]):
        for k, kind in enumerate(["int", "decimal", "float"]):
            add(7 + 3 * family + k, kind, kind, kind,
                f"({'dec' if kind == 'decimal' else kind} ({name}))")
    for op, kind in enumerate(["int", "decimal", "float", "duration"], 22):
        prim = "(time (durNeg))" if kind == "duration" else f"({'dec' if kind == 'decimal' else kind} (neg))"
        add(op, kind, "unit", kind, prim, unary=True)
    for op, a, b, ret, name in [
            (26, "duration", "duration", "duration", "durAdd"),
            (27, "duration", "duration", "duration", "durSub"),
            (28, "duration", "int", "duration", "durMul"),
            (29, "time", "duration", "time", "add"),
            (30, "time", "duration", "time", "subDur"),
            (31, "time", "time", "duration", "sub")]:
        add(op, a, b, ret, f"(time ({name}))")
    for family, name in enumerate(["eq", "lt", "le"]):
        for k, kind in enumerate(["int", "decimal", "float", "bool", "byte", "char", "time", "duration", "ordering"]):
            primitive = f"(eq ({kind}))" if family == 0 else f"(cmp ({kind}) ({name}))"
            body = None
            if kind == "ordering" and family != 0:
                body = lambda x, y, n=name: f"(prim (cmp (int) ({n})) ((lit (int {x})) (lit (int {y}))))"
            add(32 + 9 * family + k, kind, kind, "bool", primitive, body=body)

    def cmp_float(x, y):
        x, y = literal("float", x), literal("float", y)
        less, equal, greater = [f'(con (ordering) "{tag}" ())' for tag in ["Less", "Equal", "Greater"]]
        nan_x, nan_y = [f"(prim (float (isNan)) ({v}))" for v in [x, y]]
        lt, gt = [f"(prim (cmp (float) ({n})) ({x} {y}))" for n in ["lt", "gt"]]
        return f"(if_ {nan_x} (if_ {nan_y} {equal} {greater}) (if_ {nan_y} {less} (if_ {lt} {less} (if_ {gt} {greater} {equal}))))"

    add(59, "float", "float", "ordering", None, body=cmp_float)
    add(60, "bool", "unit", "bool", "(not)", unary=True)
    add(61, "int", "int", "range", "(range (make))")
    add(62, "range", "unit", "int", "(range (start))", unary=True)
    add(63, "range", "unit", "int", "(range (end))", unary=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        expected = list(pool.map(lambda path: oracle(path, world), paths))
    for driver in drivers:
        result = subprocess.run([driver, "--operations"], input="".join(inputs),
                                capture_output=True, text=True, timeout=120)
        assert result.returncode == 0, (driver, result.stderr)
        got = [line.split(" ", 1) for line in result.stdout.splitlines()]
        assert len(got) == len(expected), (len(got), len(expected))
        for i, (actual, want) in enumerate(zip(got, expected)):
            if actual[0] == "returned" and actual[1].startswith("0x"):
                if math.isnan(struct.unpack(">d", bytes.fromhex(actual[1][2:]))[0]):
                    actual[1] = "0x7ff8000000000000"
            assert actual == want, (driver, inputs[i], paths[i].read_text(), want, actual)
        print(f"vm operations: {driver}: {len(expected)} outcomes match mocore, all 57 numeric/comparison opcodes")


def structures(drivers, root, world):
    paths, inputs = [], []
    for kind in range(4):
        for a, b, c in [(17, -9, 37), (-2**63, 2**63 - 1, -2**63), (0, -17, 1)]:
            first = f"(tuple ((lit (int {a})) (lit (int {b}))))"
            second = f"(tuple ((lit (int {c})) (lit (int {b}))))"
            eq = f"(prim (eq (tuple ((int) (int)))) ({first} {second}))"
            payload = program(f"(tuple ({first} {second} {eq}))", "(tuple ((tuple ((int) (int))) (tuple ((int) (int))) (bool)))")
            path = root / f"structure-{len(paths)}.core"
            path.write_text(f"(core {VERSION} {payload})\n")
            paths.append(path)
            inputs.append(f"{kind} {a} {b} {c}\n")
    expected = [oracle(path, world) for path in paths]
    for driver in drivers:
        result = subprocess.run([driver, "--structures"], input="".join(inputs),
                                capture_output=True, text=True, timeout=60)
        assert result.returncode == 0, (driver, result.stderr)
        actual = [line.split(" ", 1) for line in result.stdout.splitlines()]
        assert actual == expected, (driver, actual, expected)
        print(f"vm structures: {driver}: {len(expected)} outcomes match mocore for equal, set_field and reset/reuse, unique/shared")


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
    # A report quotes already-quoted structural text; retain literal escapes.
    assert parse(wire(['"\0"', r'"\u{0}"'])) == ['"\0"', r'"\u{0}"']
    decimals(drivers)
    with tempfile.TemporaryDirectory(prefix="mo-vm-") as directory:
        root = pathlib.Path(directory)
        world = root / "world.core"
        world.write_text(f"(core {VERSION} {WORLD})\n")
        operations(drivers, root, world)
        structures(drivers, root, world)
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
        for i in range(256):
            body = reference_tail(random.Random(540 + i), i % 4)
            flag = "true" if i % 2 else "false"
            program = f'''(Program ((DataDecl "Pair" ((Ctor "Pair" (("a" (text)) ("b" (text))))))) (
              (FunDecl "entry" () (text) (call "choose" ((lit (bool {flag})) (lit (text "input\\u{{0}}🙂")))) "Generated" (function) 0)
              (FunDecl "choose" (("b" (bool)) ("x" (text))) (text) {body} "Generated" (function) 0)
              (FunDecl "identity" (("x" (text))) (text) (var "x") "Generated" (function) 0)
            ) () "entry" (noSource))'''
            path = root / f"reference-{i}.core"
            path.write_text(f"(core {VERSION} {program})\n")
            generated.append(path)
        for i in range(192):
            path = root / f"global-{i}.core"
            path.write_text(f"(core {VERSION} {global_program(i)})\n")
            generated.append(path)
        for i in range(384):
            payload = parse(f"(core {VERSION} {closure_program(i)})")[2]
            entry = next(f for f in payload[2] if f[1] == payload[4])
            if entry[2]:
                ty = wire(entry[3])
                value = {'(int)': '(lit (int -37))', '(text)': '(lit (text "input\\u{0}🙂"))',
                         '(bytes)': '(lit (bytes (0 127 255)))',
                         '(tuple ((text) (int)))': '(tuple ((lit (text "input")) (lit (int 31))))',
                         '(option (text))': '(con (option (text)) "Some" ((lit (text "input"))))'}[ty]
                flag = 'true' if i % 2 else 'false'
                wrapper = parse(f'(FunDecl "entry" () {ty} (call "{payload[4]}" ((lit (bool {flag})) {value})) "Generated" (function) 0)')
                payload[2].append(wrapper)
                payload[4] = "entry"
            path = root / f"closure-{i}.core"
            path.write_text(f"(core {VERSION} {wire(payload)})\n")
            generated.append(path)
        for i, (ty, literal) in enumerate([
                ("unit", "unit"), ("bool", "bool true"), ("byte", "byte 255"),
                ("int", "int -9223372036854775808"), ("float", "float 0x8000000000000000"),
                ("bytes", "bytes (0 128 255)"),
                ("char", 'char "🙂"'), ("time", "time -1"), ("duration", "duration -1"),
                ("decimal", "decimal -170141183460469231731687303715884105727")]):
            path = root / f"literal-{i}.core"
            path.write_text(f'(core {VERSION} (Program () ((FunDecl "entry" () ({ty}) (lit ({literal})) "M" (function) 0)) () "entry" (noSource)))\n')
            generated.append(path)
        # These exercise primitive dispatch through the compiled Core path.
        for name, ret, body in [
                ("text-concat", "(text)", '(prim (text (concat)) ((lit (text "left")) (lit (text "right"))))'),
                ("list-literal", "(list (int))", '(prim (list (int) (lit 2)) ((lit (int -17)) (lit (int 31))))')]:
            path = root / f"dispatch-{name}.core"
            path.write_text(f'(core {VERSION} (Program () ((FunDecl "entry" () {ret} {body} "M" (function) 0)) () "entry" (noSource)))\n')
            generated.append(path)
        tests = {}
        path = root / "dispatch-clock.core"
        path.write_text(f'''(core {VERSION} (Program () (
          (FunDecl "entry" () (unit) (lit (unit)) "M" (function) 0)
          (FunDecl "clock" () (unit)
            (let_ "clock" (prim (io (clockFixed)) ((lit (time 17))))
              (let_ "time" (prim (io (now)) ((var "clock"))) (lit (unit))))
            "M" (test) 0)) () "entry" (noSource)))\n''')
        tests[path] = "clock"
        generated.append(path)
        # A program stop may spell an internal reason; it is still a Mo stop.
        for name in ["primitive", "host"]:
            path = root / f"stop-{name}.core"
            path.write_text(f'(core {VERSION} (Program () ((FunDecl "entry" () (unit) (stop "internal error: {name} dispatch unavailable") "M" (function) 0)) () "entry" (noSource)))\n')
            generated.append(path)
        expectations = {path: oracle(path, world, tests.get(path)) for path in generated}
        for driver in drivers:
            matched = unsupported = 0
            pending = dict.fromkeys(NEEDS_DISPATCH, 0)
            for path in fixtures + generated:
                got = run(driver, path, tests.get(path))
                if got is None:
                    assert path in fixtures, ("supported generated program refused", path)
                    unsupported += 1
                    continue
                if got[0] in pending:
                    pending[got[0]] += 1
                    continue
                expected = expectations.get(path) or oracle(path, world)
                assert got == expected, (driver, path.read_text(), expected, got)
                matched += 1
            assert unsupported == CODEGEN_UNSUPPORTED, (driver, unsupported, CODEGEN_UNSUPPORTED)
            assert pending == NEEDS_DISPATCH, (driver, pending, NEEDS_DISPATCH)
            print(f"vm differential: {driver}: {matched} outcomes match mocore; {unsupported} fixtures explicitly unsupported by codegen")
            for label, count in pending.items():
                print(f"vm differential: {driver}: {label}: {count}")


if __name__ == "__main__":
    main()
