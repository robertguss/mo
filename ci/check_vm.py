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

from check_codegen import closure_program, expression, global_program, join_program, reference_tail
from check_core_check import VERSION, WORLD, Atom, parse, wire, program

NEEDS_DISPATCH = {"needs primitive dispatch": 0, "needs host dispatch": 0,
                  "needs capability fixture arguments": 0}
CODEGEN_UNSUPPORTED = 0
OWNED_OUTCOMES = 1024
NESTED_LOOP_OUTCOMES = 2


def canonical(report):
    # NaN sign/payload are not observable; retain all other bits, including signed zero.
    if report[0] == "returned" and isinstance(report[1], str) and len(report[1]) == 18 and report[1].startswith("0x"):
        if math.isnan(struct.unpack(">d", bytes.fromhex(report[1][2:]))[0]):
            report[1] = "0x7ff8000000000000"
    return report


def run(driver, path, test=None):
    args = [driver, "--core", str(path)] if test is None else [driver, "--core-test", str(path), test]
    result = subprocess.run(args, capture_output=True, text=True, timeout=15)
    if result.returncode == 77:
        return None
    assert result.returncode in (0, 78), (driver, path, result.stderr)
    assert result.stdout.endswith("\n"), (driver, path, result.stdout)
    return canonical(result.stdout[:-1].split("\n", 1))


def oracle(path, world, test=None, effects=False):
    args = ["formal/.lake/build/bin/mocore", str(path)]
    if world is not None:
        args += [str(world), "--fuel", "10000"]
    if test is not None:
        args += ["--test", test]
    result = subprocess.run(args,
                            capture_output=True, text=True, timeout=15)
    assert result.returncode in (0, 1, 2), (path, result.stdout, result.stderr)
    return outcome_of(result.stdout, effects)


def outcome_of(stdout, effects=False):
    """The comparable outcome of one `mocore` report, in the VM driver's spelling."""
    report = parse(stdout)[2]
    detail = report[1]
    if report[0] == "stopped":
        if detail[0] in ("stop", "outside"):
            detail = detail[1]
        elif detail[0] == "overflow":
            detail = ("time outside years 1 to 9999" if detail[1] == "a time outside the years 1 to 9999"
                      else f"overflow: {detail[1]}")
        else:
            detail = {"divByZero": "dividing by zero",
                      "repeatedKey": "two keys of a map literal that turn out equal"}[detail[0]]
    outcome = canonical([str(report[0]), detail])
    return (outcome, report[2]) if effects else outcome


def value_expression(value, ty, payload):
    """Run values need their parameter types to reconstruct nominal choices and empty maps."""
    tag = value[0]
    if tag == 'cap':
        raise NotImplementedError('real capability arguments need the host fixture adapter')
    if tag == 'con':
        if ty[0] == 'data':
            data = next(d for d in payload[1] if d[1] == ty[1])
            fields = next(c[2] for c in data[2] if c[1] == value[1])
            types = [field[1] for field in fields]
        elif ty[0] == 'option':
            types = [] if value[1] == 'None' else [ty[1]]
        elif ty[0] == 'result':
            types = [ty[1] if value[1] == 'Ok' else ty[2]]
        else:
            assert not value[2], (value, ty)
            types = []
        fields = ' '.join(value_expression(v, t, payload) for v, t in zip(value[2], types))
        return f'(con {wire(ty)} {wire(value[1])} ({fields}))'
    if tag == 'tuple':
        fields = ' '.join(value_expression(v, t, payload) for v, t in zip(value[1], ty[1]))
        return f'(tuple ({fields}))'
    if tag == 'map':
        fields = ' '.join(value_expression(v, t, payload)
                          for pair in value[1] for v, t in zip(pair, ty[1:]))
        return f'(prim (map {wire(ty[1])} {wire(ty[2])} (lit {len(value[1])})) ({fields}))'
    if tag in ['list', 'set']:
        fields = ' '.join(value_expression(v, ty[1], payload) for v in value[1])
        literal = f'(prim (list {wire(ty[1])} (lit {len(value[1])})) ({fields}))'
        return literal if tag == 'list' else f'(prim (set {wire(ty[1])} (fromList)) ({literal}))'
    if tag == 'range':
        return f'(prim (range (make)) ((lit (int {value[1]})) (lit (int {value[2]}))))'
    assert tag in ['int', 'bigInt', 'byte', 'decimal', 'float', 'bool', 'char', 'text', 'bytes',
                   'unit', 'time', 'duration'], (value, ty)
    return f'(lit {wire(value)})'


def entry_wrapper(payload, args):
    entry = next(f for f in payload[2] if f[1] == payload[4])
    assert len(entry[2]) == len(args), (entry[2], args)
    if not args:
        return []
    parameters, values, capabilities = [], [], []
    for i, (v, p) in enumerate(zip(args, entry[2])):
        if p[1][0] == 'cap':
            assert v[0] == 'cap' and v[1] == p[1][1], (v, p)
            name = f'%VM.cap{i}'
            parameters.append([name, p[1]])
            values.append(f'(var {wire(name)})')
            capabilities.append(v)
        else:
            values.append(value_expression(v, p[1], payload))
    assert payload[5][0] == 'noSource', 'a new helper needs compiler metadata when source is present'
    name = '%VM.entry'
    assert all(f[1] != name for f in payload[2])
    wrapper = parse(f'(FunDecl "{name}" {wire(parameters)} {wire(entry[3])} '
                    f'(call {wire(payload[4])} ({" ".join(values)})) "Generated" (function) {len(payload[3])})')
    payload[2].append(wrapper)
    payload[4] = name
    return capabilities


def line_bytes(lines):
    # Printed Text can contain LF, NUL, or no characters. Comparing bytes keeps
    # those distinctions instead of treating one print as one output line.
    return b''.join((s + '\n').encode('utf-8') for s in lines)


def folders(paths, explicit):
    out = set(explicit)
    for name in paths:
        parts = name.split('/')
        out.update('/'.join(parts[:i]) for i in range(1, len(parts)))
    return out - {''}


def fake_effects(states):
    # Maps/sets have extensional effects; list order and Random words matter.
    return [[s[1], dict(s[2]), s[3], s[4], s[5], dict(s[6]), s[7], s[8],
             folders(dict(s[6]), s[9])] for s in states]


def run_capabilities(driver, path):
    payload = parse(path.read_text())[2]
    w = payload[2]
    with tempfile.TemporaryDirectory(prefix='mo-vm-run-') as directory:
        root = pathlib.Path(directory)
        tree = root / 'files'
        tree.mkdir()
        for name in folders(dict(w[6]), w[9]):
            assert name and all(p not in ['', '.', '..'] for p in name.split('/')), name
            (tree / name).mkdir(parents=True, exist_ok=True)
        for name, text in w[6]:
            assert name and all(p not in ['', '.', '..'] for p in name.split('/')), name
            (tree / name).write_bytes(text.encode('utf-8'))
        assert all('\n' not in s and not s.endswith('\r') for s in w[3]), w[3]
        for name, lines in [('input', w[3]), ('output', w[4]), ('errors', w[5])]:
            (root / name).write_bytes(line_bytes(lines))
        result = subprocess.run([driver, '--run', str(path), directory],
                                capture_output=True, text=True, timeout=15)
        assert result.returncode == 0, (driver, path, result.stdout, result.stderr)
        body, marker, serialized = result.stdout.rpartition('\n(core ')
        assert marker and body.count('\n') >= 1, (path, result.stdout)
        outcome = canonical(body.split('\n', 1))
        snapshots = parse('(core ' + serialized)[2][5]
        actual = [(root / 'output').read_bytes(), (root / 'errors').read_bytes(),
                  {str(p.relative_to(tree)): p.read_bytes() for p in tree.rglob('*') if p.is_file()},
                  {str(p.relative_to(tree)) for p in tree.rglob('*') if p.is_dir()},
                  fake_effects(snapshots)]
    expected, effects = oracle(path, None, effects=True)
    want = [line_bytes(effects[1]), line_bytes(effects[2]),
            {name: text.encode('utf-8') for name, text in effects[3]},
            folders(dict(effects[3]), effects[4]), fake_effects(effects[5])]
    assert actual == want, (driver, path.read_text(), want, actual)
    assert outcome == expected, (driver, path.read_text(), expected, outcome)
    return outcome


def capability_cases(root):
    """Asymmetric worlds expose wrong-kind caching, lost aliases and lost effects."""
    cases = []

    def add(name, params, ret, body, values, state=None):
        payload = parse(program(body, ret, params=params))
        capabilities = entry_wrapper(payload, [parse(v) for v in values])
        world = parse(WORLD)
        world[3] = ['native first', 'native last']
        world[4] = ['native history']
        world[7] = Atom('-1')
        if state is not None:
            world[10] = [parse(state)]
        request = [Atom('Run'), payload, world, [Atom('entry'), capabilities], Atom('10000')]
        path = root / f'capability-{name}.core'
        path.write_text(f'(core {VERSION} {wire(request)})\n')
        cases.append(path)

    state = ('(CapState ("fake arg" "β") (("key" "value")) ("fake first" "fake last") '
             '("fake history") ("fake error") (("nested/a" "old")) 117 '
             '(RandomState 0x0123456789abcdef 0xfedcba9876543210 '
             '0x1111111111111111 0x9999999999999999) ("empty" "nested"))')
    for fake in [False, True]:
        cap = '(cap (console) (some "") ' + ('(some 0))' if fake else '(none))')
        for stop in [False, True]:
            ret = '(unit)' if stop else '(tuple ((option (text)) (option (text))))'
            end = '(stop "after effects")' if stop else (
                '(tuple ((prim (io (readLine)) ((var "left"))) '
                '(prim (io (readLine)) ((var "right")))))')
            body = ('(let_ "p" (prim (io (print)) ((var "left") (lit (text "line\\n\\u{0}")))) '
                    '(let_ "e" (prim (io (printError)) ((var "right") (lit (text "")))) '
                    + end + '))')
            add(f'console-{fake}-{stop}', [('left', '(cap (console))'), ('right', '(cap (console))')],
                ret, body, [cap, cap], state if fake else None)
    add('files-alias', [('write', '(cap (files))'), ('read', '(cap (readFiles))')],
        '(result (text) (fileError))',
        '(let_ "w" (prim (io (writeText)) ((var "write") (lit (text "a")) '
        '(lit (text "new🙂")))) (prim (io (readText true)) ((var "read") (lit (text "a")))))',
        ['(cap (files) (some "nested") (some 0))', '(cap (readFiles) (some "nested") (some 0))'], state)
    add('values', [('a', '(cap (args))'), ('e', '(cap (env))'), ('c', '(cap (clock))'),
                   ('r', '(cap (random))')],
        '(tuple ((list (text)) (option (text)) (time) (int)))',
        '(tuple ((prim (io (argsList)) ((var "a"))) '
        '(prim (io (envGet)) ((var "e") (lit (text "key")))) '
        '(prim (io (now)) ((var "c"))) '
        '(prim (io (randomInt)) ((var "r") (lit (int -17)) (lit (int 31))))))',
        ['(cap (args) (some "") (some 0))', '(cap (env) (some "") (some 0))',
         '(cap (clock) (some "") (some 0))', '(cap (random) (some "") (some 0))'], state)
    return cases


def permutation_program(i):
    rng = random.Random(500 + i)
    ty, values = [
        ('(int)', [f'(lit (int {n}))' for n in [-17, 31, 9, -43, 57]]),
        ('(text)', [f'(lit (text "{s}"))' for s in ['left', 'β', 'third', '', r'last\u{0}']]),
        ('(bytes)', [f'(lit (bytes ({n} 128 255)))' for n in [0, 1, 3, 17, 57]]),
        ('(tuple ((text) (int)))', [f'(tuple ((lit (text "v{n}")) (lit (int {n}))))' for n in [-17, 31, 9, -43, 57]]),
        ('(option (text))', [f'(con (option (text)) "Some" ((lit (text "v{n}"))))' for n in [0, 1, 3, 17, 57]]),
    ][i % 5]
    width = 2 if i % 3 == 0 else 3 + i % 3
    permutation = [1, 0] if width == 2 else list(range(1, width)) + [0]
    if i % 3 == 2:
        rng.shuffle(permutation)
    params = ' '.join(f'("r{j}" {ty})' for j in range(width))
    swapped = ' '.join(f'(var "r{j}")' for j in permutation)
    result = ' '.join(f'(var "r{j}")' for j in range(width))
    initial = ' '.join(values[:width])
    return f'''(Program () ((FunDecl "entry" () (tuple ({' '.join([ty] * width)}))
      (join "loop" (("n" (int)) {params})
        (if_ (prim (cmp (int) (gt)) ((var "n") (lit (int 0))))
          (jump "loop" ((prim (int (sub)) ((var "n") (lit (int 1)))) {swapped}))
          (tuple ({result})))
        (jump "loop" ((lit (int {1 + i % 7})) {initial})))
      "Generated" (function) 0)) () "entry" (noSource))'''


def owned_program(i):
    """Nested operands, repeated aliases, captures and simultaneous loop transfers."""
    concat = lambda a, b: f'(prim (text (concat)) ({a} {b}))'
    x, y = '(var "x")', '(var "y")'
    if i < 1000:
        rng = random.Random(8128 + i)

        def value(depth):
            if depth == 0:
                return rng.choice([x, y, '(lit (text ""))', '(lit (text " aβ "))'])
            choice = rng.randrange(8)
            a, b = value(depth - 1), value(depth - 1)
            if choice == 0:
                return concat(a, b)
            if choice == 1:
                return f'(let_ "y" {a} {concat(y, b)})'
            if choice == 2:
                flag = rng.choice(['true', 'false'])
                return f'(if_ (lit (bool {flag})) {a} {b})'
            if choice == 3:
                return f'(proj (tuple ({a} {b})) {rng.randrange(2)})'
            if choice == 4:
                return f'(call "id" ({a}))'
            if choice == 5:
                return f'(prim (text (trim)) ({a}))'
            if choice == 6:
                joined = concat('(var "p")', '(var "q")')
                return f'(app (lam (("p" (text)) ("q" (text))) {joined}) ({a} {b}))'
            return f'(app (lam () {concat(a, b)}) ())'

        body = value(4)
    else:
        count, keep = divmod(i - 1000, 2)
        loop = f'''(join "loop" (("a" (text)) ("b" (text)) ("n" (int)))
          (if_ (prim (eq (int)) ((var "n") (lit (int 0))))
            {concat('(var "a")', '(var "b")')}
            (jump "loop" ((var "b") {concat('(var "a")', '(lit (text "!"))')}
              (prim (int (sub)) ((var "n") (lit (int 1)))))))
          (jump "loop" ({x} {y} (lit (int {count})))))'''
        result = concat('(var "z")', x) if keep else '(var "z")'
        body = f'(let_ "z" {loop} {result})'
    body = f'''(let_ "x" {concat('(lit (text "L"))', '(lit (text "!"))')}
      (let_ "y" {concat('(lit (text "right"))', x)} {body}))'''
    return f'''(Program () (
      (FunDecl "entry" () (text) {body} "Generated" (function) 0)
      (FunDecl "id" (("p" (text))) (text) (var "p") "Generated" (function) 0)
    ) () "entry" (noSource))'''


def nested_loop_program(levels, steps):
    """Outer free references must propagate through nested backedges within the budget."""
    empty = '(lit (text ""))'
    for i in reversed(range(levels)):
        counter = f'(var "n{i}")'
        decrement = f'(prim (int (sub)) ({counter} (lit (int 1))))'
        zero = ('(prim (text (concat)) ((var "text") (lit (text "w"))))' if i == 0 else
                f'(jump "loop{i - 1}" ((prim (int (sub)) ((var "n{i - 1}") (lit (int 1))))))')
        if i == levels - 1:
            body = f'(jump "loop{i}" ({decrement}))'
            for j in range(steps):
                value = f'''(if_ (lit (bool true))
                  (prim (int (add)) ({counter} (lit (int {j}))))
                  (lit (int {-j})))'''
                body = f'(let_ "v{j}" {value} {body})'
        else:
            body = nested
        body = f'(if_ (prim (eq (int)) ({counter} (lit (int 0)))) {zero} {body})'
        # Establish a Text result even when every real path leaves the inner join.
        nested = f'''(join "loop{i}" (("n{i}" (int))) {body}
          (if_ (lit (bool false)) {empty} (jump "loop{i}" ((lit (int 1))))))'''
    body = f'''(let_ "text" (prim (text (concat)) ((lit (text "done")) (lit (text "v"))))
      {nested})'''
    return f'(Program () ((FunDecl "entry" () (text) {body} "Generated" (function) 0)) () "entry" (noSource))'


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


def structural_text(drivers, root, world):
    """Observe any type through the compiled borrowing primitive, with independent spellings.

    Float bit transport and user to_text are deliberately wrong implementations here.
    Keep the operand live after observing it to exercise borrowing under sanitizers.
    """
    cases = []

    def add(ty, expr, want, data=""):
        body = f'(let_ "value" {expr} (let_ "text" (prim (structural {ty}) ((var "value"))) ' \
               f'(prim (text (concat)) ((var "text") (prim (structural {ty}) ((var "value")))))))'
        cases.append((program(body, "(text)", data=data), None,
                      None if want is None else ["returned", want + want]))

    for ty, value, want in [
            ("unit", "unit", "Unit"), ("bool", "bool false", "false"),
            ("byte", "byte 255", "255"), ("int", "int -9223372036854775808", "-9223372036854775808"),
            ("bigInt", "bigInt 123456789012345678901234567890", "123456789012345678901234567890"),
            ("decimal", "decimal -1000000000000000001", "-1.000000000000000001"),
            ("text", 'text "raw\\n#{x}\\u{0}"', "raw\n#{x}\0"),
            ("text", 'text ""', ""), ("char", 'char "\\u{85}"', "\x85"),
            ("bytes", "bytes (0 128 255)", "bytes[0, 128, 255]"),
            ("time", "time -1", "1969-12-31T23:59:59.999999Z"),
            ("duration", "duration -1", "-0.000001s")]:
        add(f"({ty})", f"(lit ({value}))", want)
    for bits, want in [(0, "0"), (0x8000000000000000, "-0"),
                       (0x3ff0000000000000, "1"), (0x7ff0000000000000, "inf"),
                       (0xfff0000000000000, "-inf"), (0x7ff8000000000000, "nan"),
                       (0xfff8000000000001, "nan"), (1, "5e-324")]:
        add("(float)", f"(lit (float 0x{bits:016x}))", want)
    for number in [1e-7, 1e-6, 1e20, 1e21, -1e-6, -1e21]:
        for value in [math.nextafter(number, -math.inf), number, math.nextafter(number, math.inf)]:
            bits = struct.unpack(">Q", struct.pack(">d", value))[0]
            add("(float)", f"(lit (float 0x{bits:016x}))", None)
    rng = random.Random(236)
    for _ in range(128):
        add("(float)", f"(lit (float 0x{rng.getrandbits(64):016x}))", None)
    for ty, expr, want in [
            ("(tuple ((char) (char) (text)))",
             '''(tuple ((lit (char "'")) (lit (char "\\\"")) (lit (text "\\n\\u{0}\\u{85}#{x}\\\\\\\""))))''',
             '''('\\'', '"', "\\n\\u{0}\\u{85}\\#{x}\\\\\\\"")'''),
            ("(range)", "(prim (range (make)) ((lit (int 7)) (lit (int -9))))", "7..-9"),
            ("(option (text))", '(con (option (text)) "None" ())', "None"),
            ("(option (text))", '(con (option (text)) "Some" ((lit (text "x"))))', 'Some("x")'),
            ("(result (text) (int))", '(con (result (text) (int)) "Error" ((lit (int -7))))', "Error(-7)"),
            ("(ordering)", '(con (ordering) "Greater" ())', "Greater"),
            ("(fileError)", '(con (fileError) "Failed" ((lit (text "a")) (lit (text "b"))))',
             'Failed(path: "a", reason: "b")'),
            ("(entry)", '(con (entry) "Entry" ((lit (text "a")) (con (entryKind) "Link" ())))',
             'Entry(name: "a", kind: Link)'),
            ("(list (text))", '(prim (list (text) (lit 2)) ((lit (text "z")) (lit (text "a"))))',
             '["z", "a"]'),
            ("(map (text) (int))", '(prim (map (text) (int) (lit 2)) ((lit (text "z")) (lit (int 7)) (lit (text "a")) (lit (int -9))))',
             '{"z": 7, "a": -9}'),
            ("(set (int))", '(prim (set (int) (ofList)) ((prim (list (int) (lit 3)) ((lit (int 7)) (lit (int -9)) (lit (int 7))))))',
             '[7, -9].to_set'),
            ("(fn () (text))", '(let_ "capture" (lit (text "secret")) (lam () (var "capture")))', "<function>")]:
        add(ty, expr, want)
    data = '(DataDecl "Record" ((Ctor "Record" (("text" (text)) ("value" (float)))))) ' \
           '(DataDecl "Choice" ((Ctor "Empty" ()) (Ctor "With" (("record" (data "Record"))))))'
    record = '(con (data "Record") "Record" ((lit (text "field")) (lit (float 0x8000000000000000))))'
    add('(data "Record")', record, 'Record(text: "field", value: -0)', data)
    add('(data "Choice")', f'(con (data "Choice") "With" ({record}))',
        'With(record: Record(text: "field", value: -0))', data)
    for kind, constructor in [
            ("args", '(io (argsOf)) ((prim (list (text) (lit 0)) ()))'),
            ("env", '(io (envOf)) ((prim (map (text) (text) (empty)) ()))'),
            ("console", '(io (consoleFake)) ()'),
            ("files", '(io (filesInMemory)) ((prim (map (text) (text) (empty)) ()))'),
            ("readFiles", '(io (readOnly)) ((prim (io (filesInMemory)) ((prim (map (text) (text) (empty)) ()))))'),
            ("clock", '(io (clockFixed)) ((lit (time 17)))'),
            ("random", '(io (randomSeeded)) ((lit (int 31)))')]:
        ty, want = f"(cap ({kind}))", f"<{kind[0].upper() + kind[1:]}>",
        observation = f'(prim (structural {ty}) ((var "cap")))'
        condition = f'(prim (eq (text)) ({observation} (lit (text {wire(want)}))))'
        # A selected spec may create a fake. Observe it twice, and through a container too.
        nested = f'(prim (structural (tuple ({ty} (int)))) ((tuple ((var "cap") (lit (int -7))))))'
        body = f'(let_ "cap" (prim {constructor}) (if_ {condition} ' \
               f'(if_ {condition} (if_ (prim (eq (text)) ({nested} (lit (text {wire("(" + want + ", -7)")})))) ' \
               f'(lit (unit)) (stop "nested structural {kind}")) (stop "borrowed structural {kind}")) ' \
               f'(stop "structural {kind}")))'
        cases.append((program(body, "(unit)", kind="test"), "main", ["returned", "Unit"]))
    for i, (payload, test, spelling) in enumerate(cases):
        path = root / f"structural-text-{i}.core"
        path.write_text(f"(core {VERSION} {payload})\n")
        expected = oracle(path, world, test)
        if spelling is not None:
            assert expected == spelling, (path.read_text(), spelling, expected)
        assert expected[0] == "returned", (path, expected)
        for driver in drivers:
            actual = run(driver, path, test)
            assert actual == expected, (driver, path.read_text(), expected, actual)
    print(f"vm structural text: {len(cases)} cases per driver match mocore; all value kinds, D236 Float and borrowing")


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
        structural_text(drivers, root, world)
        fixtures, tests, capability_runs = [], {}, set()
        for path in sorted(pathlib.Path("tests/core").glob("*.core")):
            payload = parse(path.read_text())[2]
            if payload[0] == "Run":
                selection = payload[3]
                program = payload[1]
                if selection[0] == 'entry':
                    capabilities = entry_wrapper(program, selection[1])
                    if capabilities:
                        payload[3] = [Atom('entry'), capabilities]
                        capability_runs.add(root / path.name)
                else:
                    tests[root / path.name] = selection[1]
            else:
                program = payload
            target = root / path.name
            target.write_text(f"(core {VERSION} {wire(payload if target in capability_runs else program)})\n")
            fixtures.append(target)
        extra_capabilities = capability_cases(root)
        fixtures += extra_capabilities
        capability_runs.update(extra_capabilities)
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
        for i in range(192):
            payload = parse(f'(core {VERSION} {join_program(i)})')[2]
            entry = next(f for f in payload[2] if f[1] == payload[4])
            ty = entry[2][1][1]
            values = {
                '(int)': '(int -37)', '(text)': '(text "input\\u{0}🙂")',
                '(bytes)': '(bytes (0 127 255))',
                '(tuple ((text) (int)))': '(tuple ((text "input") (int 31)))',
                '(option (text))': '(con "Some" ((text "input")))',
                '(list (text))': '(list ((text "input") (text "second")))',
            }
            # Variant 1 loops forever when b is true: the inner join always jumps
            # back to loop, regardless of again. Use its terminating input here.
            flag = 'true' if i % 2 and i % 6 != 1 else 'false'
            if ty[0] == 'fn':
                wrapper = parse(f'(FunDecl "%VM.entry" () {wire(ty)} '
                                f'(call "start" ((lit (bool {flag})) (lam () (lit (text "input"))))) '
                                '"Generated" (function) 0)')
                payload[2].append(wrapper)
                payload[4] = '%VM.entry'
            else:
                value = '(cap (console) (some "") (some 0))' if ty[0] == 'cap' else values[wire(ty)]
                capabilities = entry_wrapper(payload, [parse(f'(bool {flag})'), parse(value)])
                if capabilities:
                    wrapper = payload[2][-1]
                    wrapper[3] = parse('(option (text))')
                    wrapper[4] = parse(f'(prim (io (readLine)) ({wire(wrapper[4])}))')
                    state = parse(WORLD)
                    state[10] = [parse('(CapState () () ("returned handle🙂" "remaining") () () () 0 (RandomState '
                                       '0x0000000000000001 0x0000000000000002 '
                                       '0x0000000000000003 0x0000000000000004) ())')]
                    payload = [Atom('Run'), payload, state, [Atom('entry'), capabilities], Atom('10000')]
            path = root / f'join-{i}.core'
            if payload[0] == 'Run':
                capability_runs.add(path)
            path.write_text(f'(core {VERSION} {wire(payload)})\n')
            generated.append(path)
        for i in range(150):
            path = root / f'permutation-{i}.core'
            path.write_text(f'(core {VERSION} {permutation_program(i)})\n')
            generated.append(path)
        for i in range(OWNED_OUTCOMES):
            path = root / f'owned-{i}.core'
            path.write_text(f'(core {VERSION} {owned_program(i)})\n')
            generated.append(path)
        nested_loops = [(85, 0), (16, 400)]
        assert len(nested_loops) == NESTED_LOOP_OUTCOMES
        for levels, steps in nested_loops:
            path = root / f'nested-loops-{levels}-{steps}.core'
            path.write_text(f'(core {VERSION} {nested_loop_program(levels, steps)})\n')
            assert oracle(path, world) == ['returned', 'donevw']
            generated.append(path)
        for i, (ty, literal) in enumerate([
                ("unit", "unit"), ("bool", "bool true"), ("byte", "byte 255"),
                ("int", "int -9223372036854775808"), ("float", "float 0x8000000000000000"),
                ("bytes", "bytes (0 128 255)"), ("text", 'text ""'),
                ("text", 'text "line\\n"'), ("float", "float 0xfff8000000000001"),
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
        expectations = {path: oracle(path, world, tests.get(path)) for path in generated
                        if path not in capability_runs}
        for driver in drivers:
            matched = unsupported = 0
            pending = dict.fromkeys(NEEDS_DISPATCH, 0)
            for path in fixtures + generated:
                if path in capability_runs:
                    run_capabilities(driver, path)
                    matched += 1
                    continue
                got = run(driver, path, tests.get(path))
                if got is None:
                    assert path in fixtures, ("supported generated program refused", path)
                    unsupported += 1
                    continue
                if got[0] in pending:
                    pending[got[0]] += 1
                    continue
                expected = expectations.get(path) or oracle(path, world, tests.get(path))
                assert got == expected, (driver, path.read_text(), expected, got)
                matched += 1
            assert unsupported == CODEGEN_UNSUPPORTED, (driver, unsupported, CODEGEN_UNSUPPORTED)
            assert pending == NEEDS_DISPATCH, (driver, pending, NEEDS_DISPATCH)
            # #71 restores nine fixtures, 24 capability joins and six capability cases.
            assert matched == 1469 + OWNED_OUTCOMES + NESTED_LOOP_OUTCOMES, (driver, matched)
            print(f"vm differential: {driver}: {matched} outcomes match mocore; {unsupported} fixtures explicitly unsupported by codegen")
            for label, count in pending.items():
                print(f"vm differential: {driver}: {label}: {count}")


if __name__ == "__main__":
    main()
