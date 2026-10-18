#!/usr/bin/env python3
"""A seeded generator of well-typed Mo Core programs, written from the typing rules in
formal/MoCore/Check.lean and the signatures in formal/TEXT.md, not from src/core/check.c.

    gen_core.py SEED [COUNT]     print COUNT programs as Run documents, one file's text each,
                                 separated by a form feed

Programs are built by choosing a type and then an expression of that type, so every one is
well typed by construction. They cover scalars, tuples, lists, maps, sets, options, results,
declared data, functions, let, if, case, join and jump, calls and the pure primitives. They use
no capability. Not covered: constants and `global`, `fail`, metadata, recursion, joins with more
than one parameter, spec and test functions, and the range, ordering, fileError and cap types; they
are for later tasks. The text is canonical: printing it again through C or Lean changes nothing.
Same seed, same programs, on every platform.
"""

import random
import sys

VERSION = "0.8"
WORLD = (
    "(World () () () () () () 0 (RandomState 0x0000000000000001 0x0000000000000001 "
    "0x0000000000000001 0x0000000000000001) () ())"
)

SCALARS = ["int", "bigInt", "byte", "decimal", "float", "bool", "char", "text", "bytes", "unit",
           "time", "duration"]
# Map keys and set members: types every Core checker accepts as keys, whatever else holds.
KEYS = ["int", "byte", "char", "text", "bool"]
INT_MAX = 2**63 - 1
DECIMAL_MAX = 2**127 - 1
# The largest count a run program repeats a text by (Gen.primitive).
REPEAT_MAX = 8


def quote(text):
    """Core's string spelling: the escapes C and Lean both print."""
    escapes = {'"': '\\"', "\\": "\\\\", "\n": "\\n", "\r": "\\r", "\t": "\\t"}
    return '"' + "".join(
        escapes.get(c, f"\\u{{{ord(c):x}}}" if ord(c) < 32 or ord(c) == 127 else c) for c in text
    ) + '"'


def sc(name):
    return f"({name})"


class Gen:
    def __init__(self, seed, lowerable=False):
        self.rng = random.Random(seed)
        # Lowering never makes a tuple of fewer than two elements and codegen refuses one with
        # E1201 (architecture 12, "Source lowering has a narrower representation contract"), so a
        # program that must run on the VM needs lowerable=True. The checkers accept both.
        self.lowerable = lowerable
        self.fresh = 0
        self.data = []  # (name, [(ctor, [(field, type)])])
        self.funs = []  # (name, [(param, type)], return type), earlier ones only

    # --- names -----------------------------------------------------------------------------

    def name(self, stem):
        self.fresh += 1
        return f"{stem}{self.fresh}"

    # --- types -----------------------------------------------------------------------------

    def key_type(self):
        return sc(self.rng.choice(KEYS))

    def type(self, depth=2, functions=True):
        r = self.rng
        if depth <= 0 or r.random() < 0.35:
            return sc(r.choice(SCALARS))
        kinds = ["tuple", "list", "map", "set", "option", "result"]
        if self.data:
            kinds += ["data", "data"]
        if functions:
            kinds.append("fn")
        kind = r.choice(kinds)
        if kind == "tuple":
            parts = [self.type(depth - 1, functions) for _ in range(r.randrange(2, 4))]
            return f"(tuple ({' '.join(parts)}))"
        if kind == "list":
            return f"(list {self.type(depth - 1, functions)})"
        if kind == "map":
            return f"(map {self.key_type()} {self.type(depth - 1, functions)})"
        if kind == "set":
            return f"(set {self.key_type()})"
        if kind == "option":
            return f"(option {self.type(depth - 1, functions)})"
        if kind == "result":
            return f"(result {self.type(depth - 1, functions)} {self.type(depth - 1, functions)})"
        if kind == "data":
            return f'(data {quote(r.choice(self.data)[0])})'
        params = [self.type(depth - 1, False) for _ in range(r.randrange(0, 3))]
        return f"(fn ({' '.join(params)}) {self.type(depth - 1, functions)})"

    def declare_data(self):
        """Non-recursive records and sums, in declaration order, each using earlier ones."""
        decls = []
        for i in range(self.rng.randrange(0, 4)):
            name = f"D{i}"
            ctors = []
            for j in range(self.rng.randrange(1, 4)):
                fields = [(self.name("f"), self.type(1, False))
                          for _ in range(self.rng.randrange(0, 3))]
                ctors.append((f"{name}C{j}", fields))
            self.data.append((name, ctors))
            decls.append(
                f'(DataDecl {quote(name)} (' + " ".join(
                    f'(Ctor {quote(c)} (' + " ".join(f'({quote(f)} {t})' for f, t in fs) + "))"
                    for c, fs in ctors) + "))")
        return decls

    # --- literals --------------------------------------------------------------------------

    def literal(self, ty):
        r = self.rng
        tag = ty[1:-1]
        edge = r.random() < 0.3
        if tag == "int":
            v = r.choice([0, 1, -1, INT_MAX, -INT_MAX - 1, 255, 256]) if edge else r.randrange(-1000, 1000)
            return f"(lit (int {v}))"
        if tag == "bigInt":
            v = r.randrange(-10**30, 10**30) if edge else r.randrange(-10**6, 10**6)
            return f"(lit (bigInt {v}))"
        if tag == "byte":
            return f"(lit (byte {r.choice([0, 255, 128]) if edge else r.randrange(256)}))"
        if tag == "decimal":
            v = r.choice([0, DECIMAL_MAX, -DECIMAL_MAX, 10**18]) if edge else r.randrange(-10**20, 10**20)
            return f"(lit (decimal {v}))"
        if tag == "float":
            bits = r.choice([0, 1 << 63, 0x7FF0000000000000, 0xFFF0000000000000,
                             0x7FF8000000000000, 0x3FF0000000000000]) if edge else r.getrandbits(64)
            return f"(lit (float 0x{bits:016x}))"
        if tag == "bool":
            return f"(lit (bool {r.choice(['true', 'false'])}))"
        if tag == "char":
            return f"(lit (char {quote(self.char())}))"
        if tag == "text":
            return f"(lit (text {quote(''.join(self.char() for _ in range(r.randrange(0, 6))))}))"
        if tag == "bytes":
            return f"(lit (bytes ({' '.join(str(r.randrange(256)) for _ in range(r.randrange(0, 5)))})))"
        if tag == "unit":
            return "(lit (unit))"
        if tag == "time":
            return f"(lit (time {r.randrange(-10**6, 10**12)}))"
        if tag == "duration":
            return f"(lit (duration {r.randrange(-10**6, 10**9)}))"
        raise AssertionError(ty)

    def char(self):
        # ASCII, controls, escapes, a Latin-1 letter, a BMP letter and an astral character.
        return self.rng.choice(["a", "Z", "0", " ", '"', "\\", "\n", "\r", "\t", "\0", "\x7f", "é", "β", "🙂"])

    # --- expressions -----------------------------------------------------------------------

    def leaf(self, ty, env):
        """Some expression of ty with no recursion: a variable of that type or a built value."""
        names = [n for n, t in env if t == ty]
        if names and self.rng.random() < 0.6:
            return f'(var {quote(self.rng.choice(names))})'
        return self.build(ty, env, 0)

    def build(self, ty, env, depth):
        """Construct a value of ty directly from its type's introduction form."""
        r = self.rng
        tag = ty.split()[0][1:].rstrip(")")
        sub = lambda t: self.expr(t, env, depth - 1, False)
        if tag in SCALARS:
            return self.literal(ty)
        inner = self.parts(ty)
        if tag == "tuple":
            return f"(tuple ({' '.join(sub(t) for t in inner)}))"
        if tag == "list":
            n = r.randrange(0, 4)
            return f"(prim (list {inner[0]} (lit {n})) ({' '.join(sub(inner[0]) for _ in range(n))}))"
        if tag == "map":
            n = r.randrange(0, 3)
            args = " ".join(sub(inner[i % 2]) for _ in range(n) for i in range(2))
            return f"(prim (map {inner[0]} {inner[1]} (lit {n})) ({args}))"
        if tag == "set":
            return f"(prim (set {inner[0]} (empty)) ())"
        if tag == "option":
            if r.random() < 0.4:
                return f"(con {ty} \"None\" ())"
            return f"(con {ty} \"Some\" ({sub(inner[0])}))"
        if tag == "result":
            i = r.randrange(2)
            return f"(con {ty} {quote(['Ok', 'Error'][i])} ({sub(inner[i])}))"
        if tag == "data":
            name = self.data_name(ty)
            ctor, fields = r.choice(dict(self.data)[name])
            return f"(con {ty} {quote(ctor)} ({' '.join(sub(t) for _, t in fields)}))"
        if tag == "fn":
            params, ret = inner
            ps = [(self.name("p"), t) for t in params]
            body = self.expr(ret, env + ps, depth - 1, False)
            return "(lam (" + " ".join(f"({quote(n)} {t})" for n, t in ps) + f") {body})"
        raise AssertionError(ty)

    def data_name(self, ty):
        return ty[len('(data "'):-2]

    def parts(self, ty):
        """The component types of a compound type, from its text."""
        toks = tokens(ty)
        tag = toks[0]
        if tag == "tuple":
            return toks[1]
        if tag in ("list", "set", "option"):
            return [toks[1]]
        if tag in ("map", "result"):
            return [toks[1], toks[2]]
        if tag == "fn":
            return toks[1], toks[2]
        return []

    def expr(self, ty, env, depth, stoppable):
        """An expression of ty. stoppable is false where the checker would have to infer a type
        for it, because a stop has none to give."""
        r = self.rng
        if depth <= 0:
            return self.leaf(ty, env)
        options = ["build", "build", "let", "if", "case", "proj", "app", "prim", "prim"]
        if stoppable:
            options += ["stop", "join"]
        callable_ = [f for f in self.funs if f[2] == ty]
        if callable_:
            options += ["call", "call"]
        choice = r.choice(options)
        d = depth - 1
        if choice == "build":
            return self.build(ty, env, depth)
        if choice == "let":
            bound_ty = self.type(1)
            name = self.name("x")
            bound = self.expr(bound_ty, env, d, False)
            return f"(let_ {quote(name)} {bound} {self.expr(ty, env + [(name, bound_ty)], d, stoppable)})"
        if choice == "if":
            return (f"(if_ {self.expr(sc('bool'), env, d, False)} {self.expr(ty, env, d, stoppable)} "
                    f"{self.expr(ty, env, d, stoppable)})")
        if choice == "case":
            return self.case(ty, env, d, stoppable)
        if choice == "proj":
            others = [self.type(1) for _ in range(r.randrange(1 if self.lowerable else 0, 3 if self.lowerable else 2))]
            at = r.randrange(len(others) + 1)
            kinds = others[:at] + [ty] + others[at:]
            parts = [self.expr(t, env, d, False) for t in kinds]
            return f"(proj (tuple ({' '.join(parts)})) {at})"
        if choice == "app":
            arg_ty = self.type(1, False)
            name = self.name("a")
            body = self.expr(ty, env + [(name, arg_ty)], d, False)
            return f"(app (lam (({quote(name)} {arg_ty})) {body}) ({self.expr(arg_ty, env, d, False)}))"
        if choice == "join":
            name, param = self.name("j"), self.name("v")
            param_ty = self.type(1, False)
            body = self.expr(ty, env + [(param, param_ty)], d, stoppable)
            jump = f"(jump {quote(name)} ({self.expr(param_ty, env, d, False)}))"
            rest = f"(if_ {self.expr(sc('bool'), env, d, False)} {jump} {self.expr(ty, env, d, stoppable)})"
            return f"(join {quote(name)} (({quote(param)} {param_ty})) {body} {rest})"
        if choice == "call":
            fname, params, _ = r.choice(callable_)
            return f"(call {quote(fname)} ({' '.join(self.expr(t, env, d, False) for _, t in params)}))"
        if choice == "prim":
            made = self.primitive(ty, env, d)
            return made if made is not None else self.build(ty, env, depth)
        return f'(stop {quote("generated " + str(r.randrange(100)))})'

    def case(self, ty, env, d, stoppable):
        r = self.rng
        kind = r.choice(["option", "result"] + (["data"] if self.data else []))
        if kind == "option":
            inner = self.type(1)
            s = self.expr(f"(option {inner})", env, d, False)
            v = self.name("o")
            return (f'(case_ {s} (("None" () {self.expr(ty, env, d, stoppable)}) '
                    f'("Some" ({quote(v)}) {self.expr(ty, env + [(v, inner)], d, stoppable)})))')
        if kind == "result":
            ok, err = self.type(1), self.type(1)
            s = self.expr(f"(result {ok} {err})", env, d, False)
            a, b = self.name("k"), self.name("e")
            return (f'(case_ {s} (("Ok" ({quote(a)}) {self.expr(ty, env + [(a, ok)], d, stoppable)}) '
                    f'("Error" ({quote(b)}) {self.expr(ty, env + [(b, err)], d, stoppable)})))')
        name, ctors = r.choice(self.data)
        s = self.expr(f"(data {quote(name)})", env, d, False)
        arms = []
        for ctor, fields in ctors:
            names = [self.name("c") for _ in fields]
            arms.append(f'({quote(ctor)} ({" ".join(quote(n) for n in names)}) '
                        f'{self.expr(ty, env + [(n, t) for n, (_, t) in zip(names, fields)], d, stoppable)})')
        return f"(case_ {s} ({' '.join(arms)}))"

    def primitive(self, ty, env, d):
        """A primitive whose result is ty, with arguments of the types its signature needs."""
        r = self.rng
        sub = lambda t: self.expr(t, env, d, False)
        tag = ty.split()[0][1:].rstrip(")")
        pick = []
        I, T, B = sc("int"), sc("text"), sc("bool")
        if ty == I:
            pick += [(f"(int ({o}))", [I, I]) for o in ("add", "sub", "mul", "min", "max", "wrappingAdd",
                                                       "saturatingMul", "floorMod")]
            pick += [("(int (neg))", [I]), ("(int (abs))", [I]), ("(text (byteCount))", [T]),
                     ("(text (charCount))", [T]), ("(bytes (size))", [sc("bytes")]),
                     ("(char (code))", [sc("char")]), ("(byte (toInt))", [sc("byte")])]
            lt = self.type(1, False)
            pick.append((f"(list {lt} (size))", [f"(list {lt})"]))
        elif ty == T:
            pick += [("(text (concat))", [T, T]), ("(text (trim))", [T]), ("(text (upper))", [T]),
                     ("(int (toText))", [I]), ("(big (toText))", [sc("bigInt")]),
                     ("(char (toText))", [sc("char")]), ("(text (replace))", [T, T, T]),
                     ("(text (repeat))", [T, I]), ("(text (join))", [f"(list {T})", T])]
        elif ty == B:
            pick += [("(not)", [B]), ("(text (isEmpty))", [T]), ("(text (contains))", [T, T]),
                     ("(float (isNan))", [sc("float")]), ("(char (isDigit))", [sc("char")])]
            for base in ("int", "text", "byte", "float", "bigInt"):
                pick.append((f"(cmp ({base}) ({r.choice(['lt', 'le', 'gt', 'ge'])}))", [sc(base)] * 2))
            lt = self.type(1, False)
            pick.append((f"(list {lt} (isEmpty))", [f"(list {lt})"]))
            k = self.key_type()
            pick.append((f"(set {k} (has))", [f"(set {k})", k]))
        elif ty == sc("bigInt"):
            pick += [("(big (add))", [ty, ty]), ("(big (mul))", [ty, ty]), ("(int (toBig))", [I])]
        elif ty == sc("float"):
            pick += [("(float (add))", [ty, ty]), ("(float (sqrt))", [ty]), ("(int (toFloat))", [I])]
        elif ty == sc("decimal"):
            pick += [("(dec (add))", [ty, ty]), ("(dec (neg))", [ty]), ("(int (toDecimal))", [I])]
        elif ty == sc("duration"):
            pick += [("(int (seconds))", [I]), ("(time (durAdd))", [ty, ty])]
        elif ty == sc("time"):
            pick += [("(time (add))", [ty, sc("duration")])]
        elif tag == "list":
            t = self.parts(ty)[0]
            pick += [(f"(list {t} (append))", [ty, t]), (f"(list {t} (prepend))", [ty, t]),
                     (f"(list {t} (concat))", [ty, ty]), (f"(list {t} (reverse))", [ty]),
                     (f"(list {t} (take))", [ty, I]), (f"(list {t} (drop))", [ty, I])]
            if t == T:
                pick += [("(text (lines))", [T]), ("(text (words))", [T])]
            k = self.key_type()
            if t == k:
                pick.append((f"(set {k} (toList))", [f"(set {k})"]))
        elif tag == "option":
            t = self.parts(ty)[0]
            pick += [(f"(list {t} (first))", [f"(list {t})"]), (f"(list {t} (last))", [f"(list {t})"]),
                     (f"(list {t} (get))", [f"(list {t})", I])]
            if t == I:
                pick += [("(text (toInt))", [T]), ("(int (checkedAdd))", [I, I])]
            k = self.key_type()
            pick.append((f"(map {k} {t} (get))", [f"(map {k} {t})", k]))
        elif tag == "map":
            k, v = self.parts(ty)
            pick += [(f"(map {k} {v} (set))", [ty, k, v]), (f"(map {k} {v} (remove))", [ty, k])]
        elif tag == "set":
            k = self.parts(ty)[0]
            pick += [(f"(set {k} (add))", [ty, k]), (f"(set {k} (remove))", [ty, k]),
                     (f"(set {k} (union))", [ty, ty]), (f"(set {k} (intersection))", [ty, ty])]
        if not pick:
            return None
        prim, args = r.choice(pick)
        parts = [sub(t) for t in args]
        if self.lowerable and prim == "(text (repeat))":
            # A run program must stay small: a count near Int's maximum makes the VM stop at
            # MO_LIMIT_VALUE_BYTES while Lean tries to build the text and runs out of time or
            # memory. Capping the count at 8 keeps negative and zero counts and the random draws.
            parts[1] = f"(prim (int (min)) ({parts[1]} (lit (int {REPEAT_MAX}))))"
        return f"(prim {prim} ({' '.join(parts)}))"

    # --- programs --------------------------------------------------------------------------

    def function(self, name, params, ret, depth):
        env = list(params)
        body = self.expr(ret, env, depth, True)
        ps = " ".join(f"({quote(n)} {t})" for n, t in params)
        decl = f'(FunDecl {quote(name)} ({ps}) {ret} {body} "Generated" (function) 0)'
        self.funs.append((name, params, ret))
        return decl

    def program(self):
        data = self.declare_data()
        funs = []
        for i in range(self.rng.randrange(0, 4)):
            params = [(self.name("a"), self.type(1, False)) for _ in range(self.rng.randrange(0, 3))]
            funs.append(self.function(f"helper{i}", params, self.type(2), self.rng.randrange(1, 5)))
        entry_type = self.type(2)
        funs.append(self.function("entry", [], entry_type, self.rng.randrange(2, 7)))
        entry = "entry"
        if self.lowerable:
            # The VM's driver spells a nested Float in decimal and Lean's report spells it in bits;
            # both engines agree on the structural observer (TEXT.md, structural values), so the
            # program returns that Text.
            funs.append(f'(FunDecl "observed" () (text) (prim (structural {entry_type}) '
                        f'((call "entry" ()))) "Generated" (function) 0)')
            entry = "observed"
        return f'(Program ({" ".join(data)}) ({" ".join(funs)}) () "{entry}" (noSource))'


def tokens(text):
    """Parse a type's text into nested lists of its component types' text."""
    pos = 0

    def span():
        nonlocal pos
        start, depth = pos, 0
        while True:
            c = text[pos]
            if c == '"':
                pos += 1
                while text[pos] != '"':
                    pos += 2 if text[pos] == "\\" else 1
            elif c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
            pos += 1
            if depth == 0:
                return text[start:pos]

    def form():
        nonlocal pos
        assert text[pos] == "("
        pos += 1
        while text[pos] == " ":
            pos += 1
        start = pos
        while text[pos] not in " )":
            pos += 1
        tag = text[start:pos]
        rest = []
        while text[pos] != ")":
            if text[pos] == " ":
                pos += 1
            elif text[pos] == "(":
                rest.append(span())
            else:
                start = pos
                while text[pos] not in " )":
                    pos += 1
                rest.append(text[start:pos])
        pos += 1
        return tag, rest

    tag, rest = form()
    if tag == "tuple":
        return [tag, tokens_list(rest[0])]
    if tag == "fn":
        return [tag, tokens_list(rest[0]), rest[1]]
    return [tag] + rest


def tokens_list(group):
    """The types inside a parenthesised list of types."""
    inner = group[1:-1].strip()
    out, depth, start, i = [], 0, 0, 0
    while i < len(inner):
        c = inner[i]
        if c == '"':
            i += 1
            while inner[i] != '"':
                i += 2 if inner[i] == "\\" else 1
        elif c == "(":
            if depth == 0:
                start = i
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                out.append(inner[start:i + 1])
        i += 1
    return out


def document(seed, lowerable=False):
    program = Gen(seed, lowerable).program()
    return f"(core {VERSION} (Run {program} {WORLD} (entry ()) 0))\n"


def main(argv):
    seed, count = int(argv[1]), int(argv[2]) if len(argv) > 2 else 1
    sys.stdout.write("\f".join(document(seed + i) for i in range(count)))


if __name__ == "__main__":
    main(sys.argv)
