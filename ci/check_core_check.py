#!/usr/bin/env python3
"""Compare public C parse/check verdicts with mocore, without evaluating program bodies.

Fixtures and every file in the committed and smoke-fuzz corpora are inputs. Programs are
wrapped as Run; non-program seeds remain negative run inputs. Generated signatures use
an independent contract table, not C's implementation. All storage is disposable.
"""

import concurrent.futures
import json
import os
import pathlib
import random
import re
import subprocess
import sys
import tempfile
import time

VERSION = re.search(
    r'#define MO_CORE_TEXT_VERSION "([^"]+)"',
    pathlib.Path("src/core/core.h").read_text(),
)[1]


class Atom(str):
    pass


def parse(source):
    tokens = re.findall(r';[^\n]*|"(?:\\.|[^"\\])*"|[()]|[^\s();"]+', source)
    tokens = [t for t in tokens if not t.startswith(";")]
    at = 0

    def item():
        nonlocal at
        token = tokens[at]
        at += 1
        if token == "(":
            result = []
            while tokens[at] != ")":
                result.append(item())
            at += 1
            return result
        if token.startswith('"'):
            token = re.sub(
                r"\\u\{([0-9a-fA-F]+)\}",
                lambda m: json.dumps(chr(int(m[1], 16)))[1:-1],
                token,
            )
            return json.loads(token)
        return Atom(token)

    result = item()
    if at != len(tokens):
        raise ValueError("trailing tokens")
    return result


def wire(value):
    if isinstance(value, list):
        return "(" + " ".join(map(wire, value)) + ")"
    if isinstance(value, Atom):
        return value
    escapes = {'"': '\\"', "\\": "\\\\", "\n": "\\n", "\r": "\\r", "\t": "\\t"}
    return (
        '"'
        + "".join(
            escapes.get(c, f"\\u{{{ord(c):x}}}" if ord(c) < 32 or ord(c) == 127 else c)
            for c in value
        )
        + '"'
    )


WORLD = (
    "(World () () () () () () 0 (RandomState "
    + " ".join(["0x0000000000000001"] * 4)
    + ") ())"
)


def run(program):
    return f"(core {VERSION} (Run {program} {WORLD} (entry ()) 0))\n".encode()


def fun(name, params, ret, body, kind="function", prefix=0):
    ps = " ".join(f"({json.dumps(n)} {t})" for n, t in params)
    return f'(FunDecl {json.dumps(name)} ({ps}) {ret} {body} "Main" ({kind}) {prefix})'


def program(
    body, ret="(int)", params=(), kind="function", extra="", consts="", data=""
):
    # Spec functions need an ordinary entry; checkProgram still checks the unused spec body.
    main = fun("main", params, ret, body, kind)
    if kind != "function":
        main = fun("entry", (), "(unit)", "(lit (unit))") + " " + main
    entry = "main" if kind == "function" else "entry"
    return f'(Program ({data}) ({main} {extra}) ({consts}) "{entry}" (noSource))'


def signatures():
    """Every current operation, with asymmetric generic types and both read-only forms."""
    table = []

    def add(family, ops, args, ret, spec=False):
        for op in [ops] if ops.startswith("lit ") else ops.split():
            table.append(
                (f"({family} ({op}))", args.split("|") if args else [], ret, spec)
            )

    I, B, D, F, T, U, H = (
        "(int)",
        "(bigInt)",
        "(decimal)",
        "(float)",
        "(text)",
        "(unit)",
        "(char)",
    )
    O = lambda t: f"(option {t})"
    L = lambda t: f"(list {t})"
    C = lambda t: f"(cap ({t}))"
    R = lambda t: f"(result {t} (fileError))"
    add(
        "int",
        "add sub mul div rem min max pow floorDiv floorMod wrappingAdd wrappingSub wrappingMul saturatingAdd saturatingSub saturatingMul",
        I + "|" + I,
        I,
    )
    add("int", "neg abs", I, I)
    add("int", "checkedAdd checkedSub checkedMul", I + "|" + I, O(I))
    for op, ret in [
        ("toText", T),
        ("toDecimal", D),
        ("toBig", B),
        ("toFloat", F),
        ("toChar", O(H)),
        ("toByte", O("(byte)")),
    ]:
        add("int", op, I, ret)
    add("int", "seconds milliseconds minutes hours days", I, "(duration)")
    add("big", "add sub mul div rem min max floorDiv floorMod", B + "|" + B, B)
    add("big", "pow", B + "|" + I, B)
    add("big", "neg abs", B, B)
    add("big", "toInt", B, O(I))
    add("big", "toText", B, T)
    add("byte", "toInt", "(byte)", I)
    add("byte", "toText", "(byte)", T)
    add("dec", "add sub mul div rem min max", D + "|" + D, D)
    add("dec", "neg abs floor ceil", D, D)
    add("dec", "round", D + "|" + I, D)
    add("dec", "toInt", D, O(I))
    add("dec", "toText", D, T)
    add("dec", "toFixed", D + "|" + I, T)
    add("dec", "toFloat", D, F)
    add("float", "add sub mul div rem", F + "|" + F, F)
    add("float", "neg abs sqrt floor ceil round", F, F)
    add("float", "isNan isInfinite", F, "(bool)")
    add("float", "toDecimal", F, O(D))
    add("float", "toText", F, T)
    table.append(("(not)", ["(bool)"], "(bool)", False))
    for (
        base
    ) in "int bigInt byte decimal float bool char text bytes time duration".split():
        for op in "lt le gt ge".split():
            table.append((f"(cmp ({base}) ({op}))", [f"({base})"] * 2, "(bool)", False))
    add("char", "code", H, I)
    add("char", "isDigit isLetter isWhiteSpace", H, "(bool)")
    add("char", "lower upper", H, H)
    add("char", "toText", H, T)
    add("text", "concat", T + "|" + T, T)
    add("text", "byteCount charCount", T, I)
    add("text", "isEmpty", T, "(bool)")
    add("text", "chars", T, L(H))
    add("text", "fromChars", L(H), T)
    add("text", "lines words", T, L(T))
    add("text", "split", T + "|" + T, L(T))
    add("text", "trim trimStart trimEnd lower upper", T, T)
    add("text", "startsWith endsWith contains", T + "|" + T, "(bool)")
    add("text", "dropPrefix dropSuffix", T + "|" + T, O(T))
    add("text", "replace", "|".join([T] * 3), T)
    add("text", "repeat padLeft padRight", T + "|" + I, T)
    for op, ret in [("toInt", I), ("toDecimal", D), ("toFloat", F)]:
        add("text", op, T, O(ret))
    add("text", "join", L(T) + "|" + T, T)
    add("text", "bytes", T, "(bytes)")
    add("bytes", "size", "(bytes)", I)
    add("bytes", "get", "(bytes)|" + I, O("(byte)"))
    add("bytes", "decodeUtf8", "(bytes)", O(T))
    add("unicode16", "graphemes", T, L(T))
    add("unicode16", "displayWidth", T, I)
    add("unicode16", "normalizeNfc lowercase uppercase", T, T)
    add("range", "make", I + "|" + I, "(range)")
    add("range", "start end", "(range)", I)
    add("range", "toList", "(range)", L(I))
    add("range", "contains", "(range)|" + I, "(bool)")
    add("range", "toText", "(range)", T)
    add("time", "sub", "(time)|(time)", "(duration)")
    add("time", "add subDur", "(time)|(duration)", "(time)")
    add("time", "checkedAdd", "(time)|(duration)", O("(time)"))
    add("time", "toText", "(time)", T)
    add("time", "parse", T, O("(time)"))
    add("time", "durAdd durSub", "(duration)|(duration)", "(duration)")
    add("time", "durMul", "(duration)|" + I, "(duration)")
    add("time", "durNeg", "(duration)", "(duration)")
    add("time", "inMilliseconds", "(duration)", I)
    add("time", "inSeconds", "(duration)", D)
    add("time", "durToText", "(duration)", T)
    t, v, li, pair = "(byte)", T, L("(byte)"), "(tuple ((byte) (text)))"
    add("list " + t, "lit 0", "", li)
    add("list " + t, "lit 2", t + "|" + t, li)
    add("list " + t, "size", li, I)
    add("list " + t, "isEmpty", li, "(bool)")
    add("list " + t, "get", li + "|" + I, O(t))
    add("list " + t, "first last", li, O(t))
    add("list " + t, "append prepend", li + "|" + t, li)
    add("list " + t, "concat", li + "|" + li, li)
    add("list " + t, "reverse unique", li, li)
    add("list " + t, "take drop", li + "|" + I, li)
    add("list " + t, "set", li + "|" + I + "|" + t, O(li))
    add("list " + t, "removeAt", li + "|" + I, O(li))
    add("list " + t, "uncons", li, O(f"(tuple ({t} {li}))"))
    add("list " + t, "enumerate", li, L(f"(tuple ({I} {t}))"))
    add("list " + t, "contains", li + "|" + t, "(bool)")
    table.append((f"(list {t} (zip {v}))", [li, L(v)], L(pair), False))
    m, se = f"(map {t} {v})", f"(set {t})"
    add("map " + t + " " + v, "lit 0", "", m)
    add("map " + t + " " + v, "lit 2", "|".join([t, v, t, v]), m)
    add("map " + t + " " + v, "empty", "", m)
    add("map " + t + " " + v, "size", m, I)
    add("map " + t + " " + v, "isEmpty", m, "(bool)")
    add("map " + t + " " + v, "get", m + "|" + t, O(v))
    add("map " + t + " " + v, "has", m + "|" + t, "(bool)")
    add("map " + t + " " + v, "set", m + "|" + t + "|" + v, m)
    add("map " + t + " " + v, "remove", m + "|" + t, m)
    add("map " + t + " " + v, "keys", m, L(t))
    add("map " + t + " " + v, "values", m, L(v))
    add("map " + t + " " + v, "entries", m, L(pair))
    add("map " + t + " " + v, "ofList", L(pair), m)
    add("set " + t, "empty", "", se)
    add("set " + t, "size", se, I)
    add("set " + t, "isEmpty", se, "(bool)")
    add("set " + t, "has", se + "|" + t, "(bool)")
    add("set " + t, "add remove", se + "|" + t, se)
    add("set " + t, "union intersection difference", se + "|" + se, se)
    add("set " + t, "toList", se, L(t))
    add("set " + t, "ofList", L(t), se)
    for eq in [I, "(fn ((int)) (text))", C("files"), '(data "undeclared")', m, se]:
        table.append((f"(eq {eq})", [eq, eq], "(bool)", False))
    add("io", "argsList", C("args"), L(T))
    add("io", "envGet", C("env") + "|" + T, O(T))
    add("io", "print printError", C("console") + "|" + T, U)
    add("io", "readLine", C("console"), O(T))
    add("io", "printed printedErrors", C("console"), L(T), True)
    for ro in ["false", "true"]:
        cap = C("files" if ro == "false" else "readFiles")
        for op, ret in [
            ("readText", R(T)),
            ("readBytes", R("(bytes)")),
            ("list", R(L(T))),
            ("entries", R(L("(entry)"))),
            ("exists", "(bool)"),
            ("within", cap),
        ]:
            table.append((f"(io ({op} {ro}))", [cap, T], ret, False))
    add("io", "writeText appendText", C("files") + "|" + T + "|" + T, R(U))
    add("io", "remove", C("files") + "|" + T, R(U))
    add("io", "readOnly", C("files"), C("readFiles"))
    add("io", "now", C("clock"), "(time)")
    add("io", "randomInt", C("random") + "|" + I + "|" + I, I)
    add("io", "consoleFake", "", C("console"), True)
    add("io", "consoleFakeInput", L(T), C("console"), True)
    add("io", "filesInMemory", f"(map {T} {T})", C("files"), True)
    add("io", "clockFixed", "(time)", C("clock"), True)
    add("io", "randomSeeded", I, C("random"), True)
    add("io", "argsOf", L(T), C("args"), True)
    add("io", "envOf", f"(map {T} {T})", C("env"), True)
    return table


def generated():
    for i, (prim, types, ret, spec) in enumerate(signatures()):
        params = [(f"a{j}", t) for j, t in enumerate(types)]
        args = [f'(var "a{j}")' for j in range(len(types))]
        body = lambda xs: f"(prim {prim} ( " + " ".join(xs) + "))"
        # Promises allow parameters and Int; discard the primitive's result in spec mode.
        wrap = lambda b: f'(let_ "discard" {b} (lit (int 0)))' if spec else b
        kind, result = ("promise", "(int)") if spec else ("function", ret)
        ps = params + [("result", "(int)")] if spec else params
        yield (
            f"signature-{i}-valid",
            run(program(wrap(body(args)), result, ps, kind)),
            0,
        )
        yield (
            f"signature-{i}-arity",
            run(program(wrap(body(args + ["(lit (unit))"])), result, ps, kind)),
            5,
        )
        for j in range(len(types)):
            wrong = list(params)
            wrong[j] = (wrong[j][0], "(bool)" if types[j] != "(bool)" else "(int)")
            if spec:
                wrong += [("result", "(int)")]
            yield (
                f"signature-{i}-arg-{j}",
                run(program(wrap(body(args)), result, wrong, kind)),
                5,
            )
        if spec:
            yield f"signature-{i}-ordinary", run(program(body(args), ret, params)), 5
        else:
            wrong_ret = "(bool)" if ret != "(bool)" else "(int)"
            yield (
                f"signature-{i}-result",
                run(program(body(args), wrong_ret, params)),
                5,
            )


def typed_trees():
    """Compose known-typed expressions at module seams, then break their result contract."""
    rng = random.Random(207)
    seeds = {
        "(int)": "(lit (int -7))",
        "(text)": '(lit (text "β"))',
        "(bool)": "(lit (bool true))",
        "(option (int))": '(con (option (int)) "Some" ((lit (int 3))))',
        "(tuple ((text) (int)))": '(tuple ((lit (text "left")) (lit (int 9))))',
        "(list (byte))": "(prim (list (byte) (lit 2)) ((lit (byte 0)) (lit (byte 255))))",
        "(fn ((text)) (bool))": '(lam (("s" (text))) (prim (text (isEmpty)) ((var "s"))))',
    }

    def expression(t, depth):
        if depth == 0:
            return seeds[t]
        a = expression(t, depth - 1)
        choice = rng.randrange(6)
        if choice == 0:
            return f'(let_ "shadow" {seeds[t]} {a})'
        if choice == 1:
            return f'(if_ (lit (bool true)) (stop "branch") {a})'
        if choice == 2:
            return f'(join "loop" (("item" {t})) {a} (jump "loop" ({seeds[t]})))'
        if choice == 3:
            return f'(proj (tuple ((lit (text "other")) {a})) 1)'
        if choice == 4:
            return f'(case_ (con (option {t}) "Some" ({a})) (("None" () (stop "none")) ("Some" ("item") (var "item"))))'
        return f'(app (lam (("item" {t})) (var "item")) ({a}))'

    for i in range(128):
        t = rng.choice(list(seeds))
        body = expression(t, rng.randrange(1, 9))
        yield f"typed-tree-{i}", run(program(body, t)), 0
        wrong = "(bool)" if t != "(bool)" else "(int)"
        yield f"typed-tree-{i}-result", run(program(body, wrong)), 5


def rule_cases():
    # These are checker contracts, not claims that WellTyped is a complete decision procedure.
    I, U = "(int)", "(unit)"
    cases = [
        ("inferred-tuple-stop", program('(tuple ((stop "x")))', "(tuple ((int)))"), 5),
        (
            "inferred-lambda-stop",
            program('(let_ "f" (lam () (stop "x")) (lit (int 1)))'),
            5,
        ),
        ("expected-lambda-stop", program('(lam () (stop "x"))', "(fn () (int))"), 0),
        (
            "non-tail-bound-jump",
            program('(join "j" () (lit (int 1)) (let_ "x" (jump "j" ()) (var "x")))'),
            5,
        ),
        (
            "non-tail-argument-jump",
            program('(join "j" () (lit (int 1)) (prim (int (neg)) ((jump "j" ()))))'),
            5,
        ),
        (
            "lambda-clears-joins",
            program('(join "j" () (stop "x") (lam () (jump "j" ())))', "(fn () (int))"),
            5,
        ),
        (
            "join-body-not-in-rest-scope",
            program('(join "j" (("x" (int))) (var "x") (var "x"))'),
            5,
        ),
        (
            "capability-capture",
            program(
                '(lam () (var "c"))', "(fn () (cap (files)))", [("c", "(cap (files))")]
            ),
            0,
        ),
        (
            "duplicate-params-first",
            program('(var "x")', I, [("x", I), ("x", "(text)")]),
            0,
        ),
        (
            "shadow-local",
            program('(let_ "x" (lit (text "new")) (var "x"))', "(text)", [("x", I)]),
            0,
        ),
        (
            "duplicate-functions-first",
            program(
                '(call "f" ())',
                I,
                extra=fun("f", (), I, "(lit (int 7))")
                + " "
                + fun("f", (), "(text)", '(lit (text "later"))'),
            ),
            0,
        ),
        (
            "check-unreachable-duplicate-body",
            program(
                '(call "f" ())',
                I,
                extra=fun("f", (), I, "(lit (int 7))")
                + " "
                + fun("f", (), "(text)", "(lit (int 8))"),
            ),
            5,
        ),
        ("unknown-nominal", program('(stop "x")', '(data "undeclared")'), 0),
        (
            "empty-data-expected-case",
            program(
                '(case_ (var "x") ())',
                I,
                [("x", '(data "Void")')],
                data='(DataDecl "Void" ())',
            ),
            0,
        ),
        (
            "empty-data-inferred-case",
            program(
                '(let_ "a" (case_ (var "x") ()) (lit (int 1)))',
                I,
                [("x", '(data "Void")')],
                data='(DataDecl "Void" ())',
            ),
            5,
        ),
        (
            "duplicate-data-first",
            program(
                '(con (data "D") "A" ())',
                '(data "D")',
                data='(DataDecl "D" ((Ctor "A" ()))) (DataDecl "D" ((Ctor "B" ())))',
            ),
            0,
        ),
        (
            "duplicate-constructors-first",
            program(
                '(con (data "D") "A" ((lit (int 7))))',
                '(data "D")',
                data='(DataDecl "D" ((Ctor "A" (("x" (int)))) (Ctor "A" (("x" (text))))))',
            ),
            0,
        ),
    ]
    # Constants are checked in order; lambdas retain the creation prefix and spec mode.
    c0 = '(ConstDecl "a" (int) (lit (int 7)))'
    c1 = '(ConstDecl "b" (int) (global "a"))'
    f = fun("main", (), I, '(global "b")', prefix=2)
    cases += [
        ("ordered-constants", f'(Program () ({f}) ({c0} {c1}) "main" (noSource))', 0),
        ("forward-constant", f'(Program () ({f}) ({c1} {c0}) "main" (noSource))', 5),
        ("duplicate-constants", program("(lit (int 0))", consts=c0 + " " + c0), 5),
        (
            "constant-spec-fake",
            program(
                "(lit (int 0))",
                consts='(ConstDecl "c" (cap (console)) (prim (io (consoleFake)) ()))',
            ),
            5,
        ),
        (
            "constant-call-prefix",
            program(
                "(lit (int 0))",
                extra=fun("f", (), I, "(lit (int 0))", prefix=1),
                consts='(ConstDecl "c" (int) (call "f" ()))',
            ),
            5,
        ),
    ]
    for kind in ["test", "example", "promise", "anchor"]:
        ret = U if kind in ("test", "example") else I
        ps = [("result", I)] if kind == "promise" else []
        body = (
            '(let_ "f" (lam () (prim (io (consoleFake)) ())) (fail (lit (text "x"))))'
        )
        cases.append((f"{kind}-lexical-lambda", program(body, ret, ps, kind), 0))
    for ty, lo, hi in [
        ("int", -(2**63), 2**63 - 1),
        ("byte", 0, 255),
        ("decimal", -(2**127 - 1), 2**127 - 1),
        ("duration", -(2**63), 2**63 - 1),
        ("time", -62135596800000000, 253402300799999999),
    ]:
        for value in [lo - 1, lo, hi, hi + 1]:
            cases.append(
                (
                    f"{ty}-{value}",
                    program(f"(lit ({ty} {value}))", f"({ty})"),
                    0 if lo <= value <= hi else 5,
                )
            )
    for label, p, expected in cases:
        yield label, run(p), expected


def paths(tree, prefix=()):
    if isinstance(tree, list):
        yield prefix, tree
        for i, value in enumerate(tree):
            yield from paths(value, prefix + (i,))


def replace(tree, path, value):
    if not path:
        return value
    result = list(tree)
    result[path[0]] = replace(tree[path[0]], path[1:], value)
    return result


def mutate(program_tree):
    # Fuzz seeds can be valid S-expressions without being codec-valid programs.
    if (
        not isinstance(program_tree, list)
        or len(program_tree) != 6
        or program_tree[0] != "Program"
        or not isinstance(program_tree[2], list)
        or any(not isinstance(f, list) or len(f) != 8 for f in program_tree[2])
    ):
        return
    p = replace(program_tree, (-1,), [Atom("noSource")])
    yield "no-source", p
    q = replace(p, (4,), "missing-entry")
    yield "entry", q
    for i, f in enumerate(p[2]):
        for field, value, label in [
            (3, [Atom("bool")], "result"),
            (6, [Atom("promise")], "kind"),
            (7, Atom("9999999999999999999999999"), "prefix"),
        ]:
            q = replace(p, (2, i, field), value)
            yield f"function-{i}-{label}", q
    expr_tags = set(
        "lit var global let_ lam app call tuple proj con case_ if_ prim stop fail join jump".split()
    )
    found = [
        (path, e)
        for path, e in paths(p)
        if e
        and isinstance(e[0], Atom)
        and e[0] in expr_tags
        and (
            (len(path) >= 3 and path[0] == 2 and path[2] == 4)
            or (len(path) >= 3 and path[0] == 3 and path[2] == 3)
        )
    ]
    # Spread mutations over whole bodies instead of repeatedly testing their first leaf.
    rng = random.Random(207)
    rng.shuffle(found)
    for i, (path, e) in enumerate(found[:48]):
        changes = [[Atom("var"), "unbound-mutant"], [Atom("stop"), "mutant"]]
        if e[0] == "prim" and len(e) == 3 and isinstance(e[2], list):
            changes.append([e[0], e[1], e[2] + [[Atom("lit"), [Atom("unit")]]]])
        if e[0] == "case_" and len(e) == 3 and isinstance(e[2], list) and e[2]:
            changes.append([e[0], e[1], list(reversed(e[2]))])
        if e[0] == "lit":
            changes.append([Atom("lit"), [Atom("int"), Atom("9223372036854775808")]])
        for j, replacement in enumerate(changes):
            q = replace(p, path, replacement)
            yield f"expr-{i}-{j}", q
    metadata = program_tree[-1]
    if isinstance(metadata, list) and len(metadata) == 5 and metadata[0] == "Metadata":
        q = replace(program_tree, (-1, 1), [])
        yield "metadata-files", q
        q = replace(program_tree, (-1, 3), [])
        yield "metadata-functions", q


def main():
    started = time.monotonic()
    drivers = sys.argv[1:]
    if not drivers:
        raise AssertionError("at least one C driver is required")
    oracle = pathlib.Path("formal/.lake/build/bin/mocore").resolve()
    cases = list(generated()) + list(typed_trees()) + list(rule_cases())
    roots = [
        pathlib.Path("tests/core"),
        pathlib.Path("fuzz/corpus/fuzz_core_text"),
        pathlib.Path("fuzz/corpus/fuzz_core_check"),
        pathlib.Path("build/fuzz/corpus/fuzz_core_text"),
        pathlib.Path("build/fuzz/corpus/fuzz_core_check"),
    ]
    originals = programs = 0
    for root in roots:
        if not root.exists():
            continue
        for path in sorted(root.rglob("*")):
            if not path.is_file():
                continue
            raw = path.read_bytes()
            # C's documented interchange bounds are outside Lean's calculus.
            if len(raw) > 8 << 20:
                raise AssertionError(f"oversize corpus input: {path}")
            try:
                doc = parse(raw.decode("utf-8"))
                payload = doc[2] if doc[:2] == [Atom("core"), Atom(VERSION)] else None
            except (ValueError, IndexError, RecursionError, UnicodeError):
                payload = None
            if payload and payload[0] in ("Run", "Program"):
                p = payload[1] if payload[0] == "Run" and len(payload) > 1 else payload
                tokens = [
                    m
                    for m in re.finditer(
                        rb';[^\n]*|"(?:\\.|[^"\\])*"|[()]|[^\s();"]+', raw
                    )
                    if not m[0].startswith(b";")
                ]
                if payload[0] == "Run":
                    # Change only a syntactically valid fuel atom, preserving all other bytes.
                    if (
                        len(payload) == 5
                        and isinstance(payload[-1], Atom)
                        and re.fullmatch(r"0|[1-9][0-9]*", payload[-1])
                    ):
                        fuel = tokens[-3]
                        raw = raw[: fuel.start()] + b"0" + raw[fuel.end() :]
                    cases.append((str(path), raw, None))
                else:
                    # Preserve original bytes, including lexical errors; do not turn a Python
                    # decoding success into an assertion that either reference codec accepts.
                    cases.append(
                        (
                            str(path),
                            run(
                                raw[tokens[3].start() : tokens[-2].end()].decode(
                                    "utf-8"
                                )
                            ),
                            None,
                        )
                    )
                # Corpus mutations include parser fuzz discoveries, not only golden fixtures.
                if "build" not in root.parts:
                    for label, q in mutate(p):
                        cases.append((str(path) + "/" + label, run(wire(q)), None))
                programs += 1
            else:
                cases.append((str(path), raw, None))
            originals += 1
    with tempfile.TemporaryDirectory(prefix="mo-core-check-") as tmp:
        files, expected = [], []
        for i, (label, raw, want) in enumerate(cases):
            path = pathlib.Path(tmp) / f"{i}.core"
            path.write_bytes(raw)
            files.append(str(path))

        def lean_verdict(item):
            path, (label, raw, want) = item
            lean = subprocess.run(
                [str(oracle), str(path)], capture_output=True, timeout=20
            )
            verdict = lean.returncode if lean.returncode in (4, 5) else 0
            if lean.returncode not in (0, 1, 2, 3, 4, 5, 6):
                raise AssertionError(
                    f"{label}: mocore failed: {lean.stdout!r} {lean.stderr!r}"
                )
            if want is not None and verdict != want:
                raise AssertionError(
                    f"{label}: generator expected {want}, Lean gave {verdict}: {raw!r} {lean.stdout!r}"
                )
            return verdict

        def c_verdict(item):
            driver, start = item
            result = subprocess.run(
                [driver] + files[start : start + 128],
                capture_output=True,
                text=True,
                check=True,
                timeout=30,
            )
            return driver, result.stdout.splitlines()

        with concurrent.futures.ThreadPoolExecutor(
            max_workers=min(8, os.cpu_count() or 2)
        ) as pool:
            expected = list(pool.map(lean_verdict, zip(files, cases)))
            jobs = [
                (driver, start)
                for driver in drivers
                for start in range(0, len(files), 128)
            ]
            results = {driver: [] for driver in drivers}
            for driver, lines in pool.map(c_verdict, jobs):
                results[driver].extend(lines)
        for driver, lines in results.items():
            if len(lines) != len(cases):
                raise AssertionError(f"{driver}: missing verdicts")
            for (label, raw, _), want, line in zip(cases, expected, lines):
                if int(line.split()[0]) != want:
                    raise AssertionError(
                        f"{driver}: {label}: Lean {want}, C {line}\n{raw.decode(errors='replace')}"
                    )
                if want == 5:
                    code = int(line.split()[1][1:])
                    if code in (1052, 1053):
                        raise AssertionError(
                            f"{driver}: {label}: resource refusal is not a typing verdict: {line}"
                        )
                    required = None
                    if label.startswith("signature-"):
                        if label.endswith("-arity"):
                            required = {1037}
                        elif "-arg-" in label or label.endswith("-result"):
                            required = {1036}
                    elif label.startswith("typed-tree-") and label.endswith("-result"):
                        required = {1036, 1038}
                    if required is not None and code not in required:
                        raise AssertionError(
                            f"{driver}: {label}: expected codes {required}, found {line}"
                        )
    print(
        f"Core checker differential: {len(cases)} cases agree in {len(drivers)} C builds; "
        f"{originals} Core corpus files ({programs} programs), all signatures and fixture mutations; "
        f"{time.monotonic() - started:.2f}s"
    )


if __name__ == "__main__":
    main()
