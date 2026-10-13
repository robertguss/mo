#!/usr/bin/env python3
"""Source/check/lower integration, after linking ci/lower_source.c with mo_check.

Run from the repository root: python3 ci/check_lower_source.py LOWER_SOURCE
TEST_VM [--require-vm]. The same lower-source driver also runs capability
entries with --core through mo_call_main. The twelve shipped examples are probed; staged
refusals are named, never counted as execution successes.
"""

import pathlib
import subprocess
import sys
import tempfile

from check_core_check import VERSION, WORLD, parse, wire
from check_vm import oracle, run


CASES = {
    "minimum": ("fn main() : Int\nif -9223372036854775808 < 0: 19 else: 1\nend", 19),
    "lazy_bool": ("fn main() : Int\n"
                  "if (true or 1 / 0 == 0) and (false implies 1 / 0 == 0): 23 else: 1\nend", 23),
    "scalar": ("fn main() : Int\n"
               "if 'λ' == '\\u{3bb}' and 0xff == 255 and 0b1010 == 10: 29 else: 1\nend", 29),
    "numeric_types": ("fn probe(a: Decimal, b: Float, c: BigInt) : Int\n"
                      "if a * 2 == -2.5 and b == 0.0 and 1.0 / b < 0.0 and c > 18446744073709551615: 31 else: 1\nend\n"
                      "fn main() : Int\nprobe(-1.25, -0.0, 0x10000000000000000)\nend", 31),
    "range_record": ("struct Pair\nleft: Int\nright: Int\nend\n"
                     "enum Choice\nValue(Pair)\nEmpty\nend\n"
                     "fn choose(value: Choice) : Int\ncase value\n"
                     "Value(Pair(left: n)): n\nEmpty: 0\nend\nend\n"
                     "fn main() : Int\nvar n = 0\nfor i in 0..10\n"
                     "continue if i % 2 == 0\nn += i\nend\n"
                     "choose(Value(Pair(right: 9, left: n)))\nend", 25),
    "field_update": ("struct Point\nx: Int\ny: Int\nend\nstruct Box\np: Point\nz: Int\nend\n"
                     "fn main() : Int\nvar b = Box(z: 7, p: Point(y: 9, x: 3))\n"
                     "b.p.x += 5\nb.p.y = 11\nb.p.x + b.p.y + b.z\nend", 26),
    "while_return": ("fn compute() : Int\nvar n = 0\nwhile n < 10\n"
                     "n += 1\ncontinue if n == 2\nreturn n + 30 if n == 5\nend\n0\nend\n"
                     "fn main() : Int\ncompute()\nend", 35),
    "list_tuple": ("fn main() : Int\nvar n = 0\nfor (a, b) in [(2, 3), (5, 7)]\n"
                   "n += a * b\nend\n(x, y) = (1, 2)\nn + x + y\nend", 44),
    "list_pattern": ("fn main() : Int\ncase [3, 5]\n[]: 1\n"
                     "[a, .._tail] if a == 2: 2\n[a, b]: a * b\n[_, .._]: 4\nend\nend", 15),
    "option_scope": ("fn main() : Int\nx = Some(7)\n"
                     "if x is Some(n) and n > 3: n + 13 else: 1\nend", 20),
    "alternatives": ("enum Choice\nFirst(Int)\nSecond(Int)\nEmpty\nend\n"
                     "fn main() : Int\ncase Second(11)\n"
                     "First(n) | Second(n) if n < 5: 1\n"
                     "First(n) | Second(n): n + 17\nEmpty: 0\nend\nend", 28),
    "alternative_guard": ("fn pick(p: (Int, Int)) : Int\ncase p\n"
                          "(n, _) | (_, n) if n > 10: n\n_: 0\nend\nend\n"
                          "fn main() : Int\npick((1, 50))\nend", 50),
    "guard_lambda": ("fn probe(n: Int, f: fn(Int) : Bool) : Bool\nf(n)\nend\n"
                     "fn pick(p: (Int, Int)) : Int\ncase p\n"
                     "(n, _) | (_, n) if probe(n, fn(x)\nx > 10\nend): n\n"
                     "_: 0\nend\nend\nfn main() : Int\npick((1, 50))\nend", 50),
    "condition_lambda": ("enum Choice\nC0\nC1\nC2\nC3\nC4\nC5\nend\n"
                         "fn main() : Int\nc = C5\nf = if c is C0\nfn(x)\nx + 1\nend\n"
                         "else if c is C1\nfn(x)\nx + 2\nend\n"
                         "else if c is C2\nfn(x)\nx + 3\nend\n"
                         "else if c is C3\nfn(x)\nx + 4\nend\n"
                         "else if c is C4\nfn(x)\nx + 5\nend\n"
                         "else\nfn(x)\nx + 6\nend\nend\nf(4)\nend", 10),
    "long_sequence": ("fn main() : Int\nvar n = 0\n" + "n += 1\n" * 80 + "n\nend", 80),
    "lambda": ("enum Choice\nFirst(Int)\nSecond(Int)\nEmpty\nend\n"
               "fn main() : Int\nf = case First(7)\n"
               "First(_) | Second(_): fn(x)\nx + 1\nend\n"
               "Empty: fn(x)\nx + 2\nend\nend\nf(4)\nend", 5),
    "generic_text": ("fn show(value: T) : Text where T: Text\n\"#{value}\"\nend\n"
                     "fn relay(value: U) : Text where U: Text\nshow(value)\nend\n"
                     "fn main() : Int\nif relay((7, false)) == \"(7, false)\": 37 else: 1\nend", 37),
    "generic_composite": ("fn show(value: T) : Text where T: Text\n\"#{value}\"\nend\n"
                          "fn relay(value: U) : Text where U: Text\nshow([value])\nend\n"
                          "fn main() : Int\nif relay(\"x\") == \"[\\\"x\\\"]\": 43 else: 1\nend", 43),
    "generic_number": ("fn add(a: T, b: T) : T where T: Number\na + b\nend\n"
                       "fn twice(value: U) : U where U: Number\nadd(value, value)\nend\n"
                       "fn main() : Int\nif twice(1.25) == 2.5: twice(23) else: 1\nend", 46),
    "generic_custom_text": ("struct Box(T)\nvalue: T\nend\n"
                            "fn to_text(box: Box(T)) : Text where T: Text\n\"<#{box.value}>\"\nend\n"
                            "fn show(value: U) : Text where U: Text\n\"#{Box(value: value)}\"\nend\n"
                            "fn main() : Int\nif show(Box(value: 7)) == \"<<7>>\": 47 else: 1\nend", 47),
    "permuted_custom_text": ("struct Pair(Z, A)\nfirst: Z\nsecond: A\nend\n"
                             "fn to_text(pair: Pair(U, T)) : Text where U: Text, T: Text\n"
                             "\"#{pair.first}/#{pair.second}\"\nend\n"
                             "fn main() : Int\nif Pair(first: true, second: 7).to_text == \"true/7\": 49 else: 1\nend", 49),
    "generic_custom_order": ("struct Box(T)\nvalue: T\nend\n"
                             "fn compare(a: Box(T), b: Box(T)) : Ordering where T: Order\n"
                             "if a.value < b.value: Less else: Greater\nend\n"
                             "fn below?(a: U, b: U) : Bool where U: Order\na < b\nend\n"
                             "fn main() : Int\nif below?(Box(value: 2), Box(value: 7)): 53 else: 1\nend", 53),
    "generic_float_order": ("fn above?(a: T, b: T) : Bool where T: Order\na > b\nend\n"
                            "fn probe(zero: Float) : Int\nif above?(zero / zero, 1.0): 1 else: 59\nend\n"
                            "fn main() : Int\nprobe(0.0)\nend", 59),
    "recursive_generic_text": ("enum Chain(T)\nEmpty\nMore(value: T, rest: Chain(T))\nend\n"
                               "fn empty() : Chain(Int)\nEmpty\nend\n"
                               "fn show(value: U) : Text where U: Text\n\"#{value}\"\nend\n"
                               "fn main() : Int\nif show(More(value: 7, rest: More(value: 9, rest: empty()))) == "
                               "\"More(value: 7, rest: More(value: 9, rest: Empty))\": 61 else: 1\nend", 61),
    "generic_function_value": ("fn show(value: T) : Text where T: Text\n\"#{value}\"\nend\n"
                               "fn accept(f: fn(Int) : Text) : Int\nif f(17) == \"17\": 67 else: 1\nend\n"
                               "fn main() : Int\naccept(show)\nend", 67),
    "try_wrapper_requirements": ("enum Inner(T)\nEmptyInner\nBadInner(Set(T))\nend\n"
                                 "enum Outer(U)\nWrapped(Inner(U))\nend\n"
                                 "fn empty() : Inner(Int)\nEmptyInner\nend\n"
                                 "fn lift(value: Result(Int, Inner(T))) : Result(Text, Outer(T)) where T: Hash\n"
                                 "number = try value\nOk(\"#{number}\")\nend\n"
                                 "fn main() : Int\nif lift(Error(empty())) is Error(Wrapped(EmptyInner)): 79 else: 1\nend", 79),
    "helper_and_builtin_text": ("fn to_text(n: Int) : Int\nn + 3\nend\n"
                                "fn compare(a: Int, b: Int) : Bool\na > b\nend\n"
                                "fn main() : Int\nif compare(7, 2) and 1 < 2: to_text(7) + 29.to_text.byte_count else: 1\nend", 12),
    "native_scalar_text": ("fn probe(b: Byte, big: BigInt, d: Decimal, f: Float) : Int\n"
                           "if b.to_text == \"255\" and big.to_text == \"18446744073709551616\" "
                           "and d.to_text == \"-1.25\" and f.to_text == \"-0\" "
                           "and 'λ'.to_text.byte_count == 2: 83 else: 1\nend\n"
                           "fn time_text(t: Time) : Text\nt.to_text\nend\n"
                           "fn duration_text(d: Duration) : Text\nd.to_text\nend\n"
                           "fn main() : Int\nprobe(255, 0x10000000000000000, -1.25, -0.0)\nend", 83),
    "native_text_function_values": ("fn main() : Int\nrender = Int.to_text\ncount = Text.byte_count\n"
                                    "count(render(-29)) + Text.byte_count(Int.to_text(29)) "
                                    "+ count(\"λ日本\")\nend", 13),
    "option_default_lazy": ("fn main() : Int\na = Some(73) or 1 / 0\n"
                            "b = None or 7\nc = (Some(Some(13)) or None) or 1 / 0\na + b + c\nend", 93),
    "option_default_vars": ("fn choose(value: Option(Int)) : Option(Int)\nvar n = 3\n"
                            "picked = value or try None\nn += picked\nSome(n)\nend\n"
                            "fn main() : Int\n(choose(Some(41)) or 1) + (choose(None) or 7)\nend", 51),
    "nested_try_option": ("fn peel(value: Option(Option(Int))) : Option(Int)\nSome(try try value)\nend\n"
                          "fn main() : Int\n(peel(Some(Some(15))) or 1) + "
                          "(peel(Some(None)) or 20) + (peel(None) or 30)\nend", 65),
    "try_nearest_lambda": ("fn probe(f: fn(Option(Int)) : Option(Text)) : Int\n"
                           "if (f(None) or \"fallback\") == \"fallback\" "
                           "and (f(Some(12)) or \"\") == \"12\": 85 else: 1\nend\n"
                           "fn main() : Int\nprobe(fn(value)\nSome(\"#{try value}\")\nend)\nend", 85),
    "try_direct_result_text": ("fn lift(value: Result(Int, Text)) : Result(Text, Text)\n"
                               "n = try value\nOk(n.to_text)\nend\n"
                               "fn measure(value: Result(Int, Text)) : Int\ncase lift(value)\n"
                               "Ok(s): s.byte_count\nError(s): 10 * s.byte_count\nend\nend\n"
                               "fn main() : Int\nmeasure(Ok(17)) + measure(Error(\"λ日本\"))\nend", 82),
    "try_builtin_wrapper": ("fn lift(value: Result(Int, Int)) : Result(Text, Option(Int))\n"
                             "n = try value\nOk(n.to_text)\nend\n"
                             "fn main() : Int\ncase lift(Error(9))\n"
                             "Error(Some(n)): n + 80\nError(None): 2\nOk(_): 1\nend\nend", 89),
}

SPEC_CASES = {
    "examples": ("fn plus(value: Int) : Int\nexamples\nplus(3) == 4\nend\n"
                 "examples\nplus(8) == 10\nend\nvalue + 1\nend\n"
                 "fn main() : Int\n17\nend", {
                     None: ["returned", "17"],
                     "Main.plus.example.1": ["returned", "Unit"],
                     "Main.plus.example.2": ["failed", "left 1:9,right 2:10,"],
                 }),
    "generic_examples": ("fn twice(value: T) : T where T: Number, T: Equal\nexamples\n"
                         "twice(3) == 6\ntwice(1.25) == 2.5\nend\nvalue + value\nend\n"
                         "fn main() : Int\n17\nend", {
                             "Main.twice.example.1": ["returned", "Unit"],
                             "Main.twice.example.2": ["returned", "Unit"],
                         }),
    "tuple_promise": ("fn ordered(a: Int, b: Int) : (Int, Int)\n"
                      "promise \"values stay\\nordered\"\nresult.0 <= result.1\n"
                      "result == (a, b) or result == (b, a)\n"
                      "allows result: (2, 5), b: 2, a: 5\n"
                      "forbids a: 5, result: (5, 2), b: 2\n"
                      "forbids result: (2, 2), b: 2, a: 5\nend\n"
                      "if a <= b: (a, b) else: (b, a)\nend\nfn main()\nend", {
                          "Main.ordered.promise.1.allows.1": ["returned", "0"],
                          "Main.ordered.promise.1.forbids.1": ["returned", "1"],
                          "Main.ordered.promise.1.forbids.2": ["returned", "2"],
                      }),
    "generic_requirements": ("fn twice(value: T) : T where T: Number, T: Equal, T: Order\n"
                             "promise \"bounded double\"\nresult > value\n"
                             "result == value + value\nallows result: 6, value: 3\n"
                             "allows value: 4, result: 8\nforbids result: 2, value: 3\n"
                             "forbids value: 1.25, result: 2.0\nend\nvalue + value\nend\n"
                             "fn main() : Int\n17\nend", {
                                 "Main::Main.twice(Int).promise.1.allows.1": ["returned", "0"],
                                 "Main::Main.twice(Int).promise.1.allows.2": ["returned", "0"],
                                 "Main::Main.twice(Int).promise.1.forbids.1": ["returned", "1"],
                                 "Main::Main.twice(Decimal).promise.1.forbids.2": ["returned", "2"],
                             }),
    "lazy_promise": ("fn identity(value: Int) : Int\npromise \"lazy conditions\"\n"
                     "result == value\n1 / 0 == 0\nallows value: 3, result: 4\n"
                     "forbids result: 3, value: 3\nend\nvalue\nend\nfn main()\nend", {
                         "Main.identity.promise.1.allows.1": ["returned", "1"],
                         "Main.identity.promise.1.forbids.1": ["stopped", "dividing by zero"],
                     }),
    "anchor_order": ("fn first() : Int\nstop \"result first\"\nend\n"
                     "fn second() : Int\nstop \"value second\"\nend\n"
                     "fn identity(value: T) : T where T: Equal\n"
                     "promise \"written argument order\"\nresult == value\n"
                     "allows result: 3, value: 3\n"
                     "forbids result: first(), value: second()\n"
                     "end\nvalue\nend\nfn main()\nstop \"main is separate\"\nend", {
                         "Main::Main.identity(Int).promise.1.allows.1": ["returned", "0"],
                         "Main::Main.identity(Int).promise.1.forbids.1": ["stopped", "result first"],
                     }),
    "scoped_text": ("struct Box(T)\nvalue: T\nend\n"
                    "fn to_text(box: Box(T)) : Text where T: Text\n\"<#{box.value}>\"\nend\n"
                    "fn relay(value: U) : Text where U: Text\nvalue.to_text\nend\n"
                    "fn show(value: T) : Text where T: Text\n"
                    "promise \"uses the selected renderer\"\nresult == relay(value)\n"
                    "allows result: \"<7>\", value: Box(value: 7)\n"
                    "forbids value: Box(value: \"x\"), result: \"wrong\"\nend\n"
                    "relay(value)\nend\nfn main()\nend", {
                        "Main::Main.show(Main.Box(Int)).promise.1.allows.1": ["returned", "0"],
                        "Main::Main.show(Main.Box(Text)).promise.1.forbids.1": ["returned", "1"],
                    }),
    "permuted_keys": ("fn mixed(left: Z, right: A) : (Z, A) "
                      "where Z: Number, Z: Equal, A: Equal\n"
                      "promise \"keeps the second and doubles the first\"\n"
                      "result.0 == left + left\nresult.1 == right\n"
                      "allows right: \"λ\", result: (6, \"λ\"), left: 3\n"
                      "forbids result: (2.5, false), left: 1.25, right: true\nend\n"
                      "(left + left, right)\nend\nfn main()\nend", {
                          "Main::Main.mixed(Text, Int).promise.1.allows.1": ["returned", "0"],
                          "Main::Main.mixed(Bool, Decimal).promise.1.forbids.1": ["returned", "2"],
                      }),
    "custom_order": ("struct Box\nvalue: Int\nend\n"
                     "fn compare(a: Box, b: Box) : Ordering\n"
                     "if a.value < b.value: Greater else: Less\nend\n"
                     "fn pick(a: T, b: T) : T where T: Order, T: Equal\n"
                     "promise \"uses reverse ordering\"\n"
                     "if a < b: result == a else: result == b\n"
                     "allows result: Box(value: 7), a: Box(value: 2), b: Box(value: 7)\n"
                     "forbids b: Box(value: 7), result: Box(value: 2), a: Box(value: 2)\n"
                     "end\nif a < b: a else: b\nend\nfn main()\nend", {
                         "Main::Main.pick(Main.Box).promise.1.allows.1": ["returned", "0"],
                         "Main::Main.pick(Main.Box).promise.1.forbids.1": ["returned", "1"],
                     }),
    "proof_free_details": ("struct Box\nvalue: Int\nend\n"
                           "fn to_text(_box: Box) : Text\nstop \"must not render\"\nend\n"
                           "fn same(a: T, b: T) : Bool where T: Equal\nexamples\n"
                           "Box(value: 7) == Box(value: 9)\nend\na == b\nend\n"
                           "test \"Unicode details\\u{0}\"\nassert \"λ\" == \"日本\"\nend\n"
                           "fn main()\nend", {
                               "Main.same.example.1": ["failed", "left 13:Box(value: 7),right 13:Box(value: 9),"],
                               "Main.test.1": ["failed", "left 2:λ,right 6:日本,"],
                           }),
    "late_instances": ("fn keep(value: T) : T where T: Equal\n"
                       "promise \"keeps its input\"\nresult == value\n"
                       "allows value: 3, result: 3\nforbids result: 4, value: 3\nend\n"
                       "value\nend\ntest \"spec-only generic use\"\n"
                       "assert keep((\"λ\", false)) == (\"λ\", false)\nend\nfn main()\nend", {
                           "Main.test.1": ["returned", "Unit"],
                           "Main::Main.keep(Int).promise.1.allows.1": ["returned", "0"],
                           "Main::Main.keep(Int).promise.1.forbids.1": ["returned", "1"],
                       }),
    "promise_ordinals": ("fn keep(value: T) : T where T: Equal\n"
                         "promise \"keeps input\"\nresult == value\n"
                         "allows value: 3, result: 3\nforbids result: 4, value: 3\nend\n"
                         "promise \"changes input\"\nresult != value\n"
                         "allows result: 4, value: 3\nforbids value: 3, result: 3\nend\n"
                         "value\nend\nfn main()\nend", {
                             "Main::Main.keep(Int).promise.1.allows.1": ["returned", "0"],
                             "Main::Main.keep(Int).promise.2.allows.1": ["returned", "0"],
                             "Main::Main.keep(Int).promise.1.forbids.1": ["returned", "1"],
                             "Main::Main.keep(Int).promise.2.forbids.1": ["returned", "1"],
                         }),
}

for count in (4, 8, 16):
    CASES[f"linear_bool_{count}"] = (
        "fn main() : Int\nx = false\n" + "\n".join(
            ("if" if i == 0 else "else if") + " x and x\n7" for i in range(count))
        + "\nelse\n41\nend\nend", 41)
    CASES[f"linear_pattern_{count}"] = (
        "enum Choice\n" + "\n".join(f"C{i}" for i in range(17)) + "\nend\n"
        "fn main() : Int\nx = C16\n" + "\n".join(
            ("if" if i == 0 else "else if") + f" x is C{i}\n7" for i in range(count))
        + "\nelse\n41\nend\nend", 41)


def check_specs(driver, vm, root, world):
    checked = 0
    for name, (body, selections) in SPEC_CASES.items():
        source = root / "main.mo"
        original = ("module Main\n" + body + "\n").encode()
        source.write_bytes(original)
        report = subprocess.run([str(driver), str(source)], capture_output=True,
                                text=True, timeout=30)
        assert report.returncode == 0, (name, report.returncode, report.stderr)
        path = root / f"spec-{name}.core"
        path.write_text(report.stdout)
        document = parse(report.stdout)
        program = document[2]
        metadata = program[-1]
        assert metadata[0] == "Metadata"
        names = [fun[1] for fun in program[2]]
        assert len(names) == len(set(names)), (name, "duplicate spec/instance names")
        promises = []
        for fun, meta in zip(program[2], metadata[3]):
            kind = fun[6][0]
            if kind == "promise":
                promises.append(fun[1])
                subject = program[2][int(meta[5][1])]
                assert [p[1] for p in fun[2]] == [p[1] for p in subject[2]] + [subject[3]], name
                start = int(meta[2][1][2])
                header = original[start:].split(b"\n", 1)[0].decode()
                assert meta[3] == ["some", header] and header.startswith("promise "), name
                for line in meta[7]:
                    span = line[1][1]
                    assert line[2].encode() == original[int(span[2]):int(span[3])], (name, line)
            elif kind == "anchor":
                assert program[2][int(meta[5][1])][6][0] == "promise" and not fun[2], name
            elif kind == "test" and name == "proof_free_details":
                assert meta[3] == ["some", "Unicode details\0"], meta[3]
        for module in metadata[2]:
            spans = [int(metadata[3][int(i)][2][1][2]) for i in module[3]
                     if program[2][int(i)][6][0] != "function"]
            assert spans == sorted(spans), (name, "discovery changed spec source order")
        if name == "generic_requirements":
            assert len(promises) == 2, (name, "same subject/type list must share its promise")
        if name == "late_instances":
            assert "Main::Main.keep((Text, Bool)).promise.1" in promises, promises
        for target, expected in selections.items():
            actual_target, oracle_path = target, path
            anchor = target is not None and any(
                f[1] == target and f[6][0] == "anchor" for f in program[2])
            if anchor:
                # Lean selects Unit specs, not Int anchors. A Unit observation
                # adapter reports the answer as failure Text. The VM executes
                # the original checked source document with all metadata intact.
                probe = parse(report.stdout)
                payload = probe[2]
                actual_target = "%lower.source.anchor.answer"
                payload[2].append(parse(f'''(FunDecl {wire(actual_target)} () (unit)
                    (fail (prim (int (toText)) ((call {wire(target)} ()))))
                    "Main" (test) {len(payload[3])})'''))
                payload[5] = parse("(noSource)")
                oracle_path = root / "source-anchor-probe.core"
                oracle_path.write_text(wire(probe) + "\n")
            actual = oracle(oracle_path, world, actual_target)
            if anchor and actual[0] == "failed":
                actual[0] = "returned"
            assert actual == expected, (name, target, expected, actual)
            runner = vm if target is None else vm.parent / "tests/test_vm.test"
            actual = run(str(runner), path, target)
            assert actual == expected, (name, target, expected, actual)
            checked += 1
    print(f"lower source specs: {len(SPEC_CASES)} real programs; {checked} expected "
          "outcomes match Lean and VM; 0 refusals")


def main():
    driver, vm = map(pathlib.Path, sys.argv[1:3])
    require_vm = "--require-vm" in sys.argv
    matched = refused = 0
    lengths = {}
    with tempfile.TemporaryDirectory(prefix="mo-lower-source-") as directory:
        root = pathlib.Path(directory)
        world = root / "world.core"
        world.write_text(f"(core {VERSION} {WORLD})\n")
        for name, (body, status) in CASES.items():
            source = root / "main.mo"
            source.write_text("module Main\n" + body + "\n")
            result = subprocess.run([str(driver), str(source)], capture_output=True,
                                    text=True, timeout=30)
            assert result.returncode == 0, (name, result.returncode, result.stderr)
            path = root / f"{name}.core"
            path.write_text(result.stdout)
            lengths[name] = len(result.stdout.encode())
            metadata = parse(result.stdout)[2][-1]
            assert metadata[0] == "Metadata"
            names = [closure[1] for fun in metadata[3] for closure in fun[4][4]]
            assert len(names) == len(set(names)), (name, "duplicate closure display names", names)
            expected = ["returned", str(status)]
            actual = oracle(path, world)
            assert actual == expected, (name, expected, actual)
            actual = run(str(vm), path)
            if actual is None:
                refused += 1
                assert name in {"lazy_bool", "numeric_types", "range_record", "field_update", "while_return", "list_tuple",
                                "list_pattern", "alternatives", "alternative_guard", "guard_lambda",
                                "condition_lambda", "lambda", "linear_pattern_4",
                                "linear_pattern_8", "linear_pattern_16", "linear_bool_4",
                                "linear_bool_8", "linear_bool_16"}, (name, "unexpected refusal")
                assert not require_vm, (name, "compiler or host refused")
            else:
                assert actual == expected, (name, expected, actual)
                matched += 1
        check_specs(driver, vm, root, world)
        for path in sorted(pathlib.Path("examples").glob("*.mo")):
            result = subprocess.run([str(driver), str(path.resolve())], capture_output=True,
                                    text=True, timeout=30)
            if result.returncode == 0:
                parse(result.stdout)  # the driver already asserts Core's checker verdict
                print(f"example {path.name}: lowered and checked; needs its input/host scenario")
            else:
                assert result.returncode == 1 and result.stderr.startswith("E"), (path, result)
                print(f"example {path.name}: staged refusal: {result.stderr.splitlines()[0]}")
    for family in ("bool", "pattern"):
        sizes = [lengths[f"linear_{family}_{n}"] for n in (4, 8, 16)]
        assert all(b <= 3 * a for a, b in zip(sizes, sizes[1:])), (family, sizes)
        print(f"lower source: linear {family} sizes at 4/8/16 branches: {sizes}")
    print(f"lower source: {len(CASES)} expected outcomes match Lean; "
          f"{matched} VM outcomes match; {refused} compiler/host refusals")


if __name__ == "__main__":
    main()
