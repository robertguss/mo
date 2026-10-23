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

LIBRARY = "lib"  # the repository's lib/, relative to the working directory ci/check.sh runs from


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
    "helper_and_builtin_text": ("fn render_number(n: Int) : Int\nn + 3\nend\n"
                                "fn larger(a: Int, b: Int) : Bool\na > b\nend\n"
                                "fn main() : Int\nif larger(7, 2) and 1 < 2: render_number(7) + 29.to_text.byte_count else: 1\nend", 12),
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
    "constant_diamond": ("answer = left + right\nleft = base * 2\nright = base + 19\n"
                         "base = 13\nfn main() : Int\nanswer\nend", 58),
    "constant_mutual_calls": ("answer = first(4)\nbase = 13\n"
                              "fn first(n: Int) : Int\nif n == 0: base else: second(n - 1)\nend\n"
                              "fn second(n: Int) : Int\nfirst(n)\nend\n"
                              "fn main() : Int\nanswer + 24\nend", 37),
    "constant_function_calls": ("base = 13\nhandler = first\nanswer = handler()\n"
                                "reader = fn() Some(try Some(1)) end\n"
                                "fn first() : Int\nbase\nend\n"
                                "fn main() : Int\nanswer + (reader() or 7)\nend", 14),
    "constant_unused_requirement": ("answer = ignore(Label(value: 3))\nstruct Label\nvalue: Int\nend\n"
                                    "fn to_text(_label: Label) : Text\nanswer.to_text\nend\n"
                                    "fn ignore(_value: T) : Int where T: Text\n42\nend\n"
                                    "fn main() : Int\nanswer\nend", 42),
    "constant_custom_text": ("answer = render()\nbase = \"label\"\nstruct Label\nvalue: Int\nend\n"
                             "fn to_text(label: Label) : Text\n\"#{base}#{label.value}\"\nend\n"
                             "fn render() : Text\n\"#{(Label(value: 3), [Label(value: 4)])}\"\nend\n"
                             "fn main() : Int\nif answer == \"(label3, [label4])\": 37 else: 1\nend", 37),
    "constant_custom_order": ("answer = Label(value: 3) < Label(value: 4)\nbase = Less\n"
                              "struct Label\nvalue: Int\nend\n"
                              "fn compare(_a: Label, _b: Label) : Ordering\nbase\nend\n"
                              "fn main() : Int\nif answer: 43 else: 1\nend", 43),
    "constant_chain": ("\n".join(f"c{i} = c{i + 1} + 1" for i in range(64)) +
                       "\nc64 = 3\nfn main() : Int\nc0\nend", 67),
    "native_abs_constant": ("answer = handler(-7)\nhandler = Int.abs\n"
                            "fn main() : Int\nanswer + 30\nend", 37),
    "native_abs_routes": ("fn apply(f: fn(Int) : Int, n: Int) : Int\nf(n)\nend\n"
                          "fn main() : Int\nn = -17\nhandler = Int.abs\n"
                          "if Int.abs(n) == 17 and n.abs == 17 and handler(-17) == 17 "
                          "and handler(0) == 0 and handler(19) == 19 "
                          "and apply(Int.abs, -23) == 23: 41 else: 1\nend", 41),
    "native_abs_boundary": ("fn main() : Int\nhandler = Int.abs\n"
                            "if handler(-9223372036854775807) == 9223372036854775807 "
                            "and Int.abs(9223372036854775807) == 9223372036854775807: 47 else: 1\nend", 47),
    "native_min_max_routes": ("low = Int.min\nhigh = Int.max\n"
                              "fn apply(f: fn(Int, Int) : Int, a: Int, b: Int) : Int\nf(a, b)\nend\n"
                              "fn main() : Int\na = -11\n"
                              "if Int.min(7, a) == -11 and a.min(7) == -11 "
                              "and low(7, a) == -11 and low(a, 7) == -11 "
                              "and high(7, a) == 7 and a.max(7) == 7 and Int.max(a, 7) == 7 "
                              "and apply(Int.max, -8, -19) == -8 "
                              "and apply(Int.min, 0, -9) == -9: 53 else: 1\nend", 53),
    "native_min_max_boundary": ("fn main() : Int\na = -9223372036854775808\nb = 9223372036854775807\n"
                                "if Int.max(a, b) == b and Int.min(b, a) == a "
                                "and Int.min(0, a) == a and Int.max(b, 0) == b: 59 else: 1\nend", 59),
    "native_alias_chain": ("handler = Int.abs\nalias = handler\nanswer = alias(-5) + alias(19) + handler(0)\n"
                          "fn main() : Int\nanswer + 13\nend", 37),
    "capture_snapshot": ("fn main() : Int\nvar count = 7\ncurrent = count\n"
                         "f = fn() current end\ncount += 19\ng = fn() count end\nf() * 10 + g()\nend", 96),
    "capture_returned_parameter": ("fn make(value: Int) : fn() : Int\nfn() value + value end\nend\n"
                                   "fn main() : Int\nmake(19)()\nend", 38),
    "capture_nested_lifetimes": ("fn make(left: Int, right: Int) : fn(Int) : fn() : Int\n"
                                 "fn(shift)\ntemp = left * 7\nfn() right + temp + shift + left + right end\nend\nend\n"
                                 "fn main() : Int\nmake(3, 29)(5)()\nend", 87),
    "capture_written_order": ("fn invoke(f: fn() : Int, extra: Int) : Int\nf() + extra\nend\n"
                              "fn make(left: Int, right: Int) : fn() : Int\n"
                              "fn() invoke(fn() right + left + right end, left) end\nend\n"
                              "fn main() : Int\nmake(7, 23)()\nend", 60),
    "capture_exclusive_scopes": ("fn select(flag: Bool) : fn() : Int\nif flag\nitem = 13\nfn() item end\n"
                                 "else\nitem = 29\nfn() item end\nend\nend\n"
                                 "fn main() : Int\nselect(true)() * 10 + select(false)()\nend", 159),
    "capture_exclusive_writes": ("fn make(flag: Bool) : fn() : Int\nvar count = 7\nif flag\nfn() count end\n"
                                 "else\ncount += 19\nfn() count end\nend\nend\n"
                                 "fn main() : Int\nmake(true)() + make(false)()\nend", 33),
    "capture_copied_record": ("struct Box\nvalue: Int\nend\nfn main() : Int\nvar box = Box(value: 3)\n"
                              "copy = box\nf = fn() copy.value end\nbox.value += 19\nf() * 10 + box.value\nend", 52),
    "capture_text_lifetime": ("fn make(prefix: Text) : fn(Int) : Text\nfn(n) \"#{prefix}/#{n}\" end\nend\n"
                              "fn main() : Int\nif make(\"λ\")(17) == \"λ/17\": 43 else: 1\nend", 43),
    "capture_generic_number": ("fn make(value: T) : fn(T) : fn() : T where T: Number\n"
                               "fn(extra) fn() value + extra end end\nend\n"
                               "fn main() : Int\nif make(1.25)(2.5)() == 3.75: make(13)(29)() else: 1\nend", 42),
    "capture_nearest_try": ("fn make(base: Int) : fn(Option(Int)) : Option(Int)\n"
                            "fn(value) Some(base + try value) end\nend\n"
                            "fn main() : Int\naction = make(13)\n"
                            "if action(Some(29)) == Some(42) and (action(None) or 7) == 7: 47 else: 1\nend", 47),
    "capture_per_round": ("fn main() : Int\nvar total = 0\nfor n in 0..3\nvar count = n\ncount += 1\n"
                          "show = fn() count end\ntotal += show()\nend\ntotal\nend", 6),
    "capture_capability_parameter": ("fn make() : fn(Console)\nfn(console) console.print(\"ok\") end\nend\n"
                                    "fn main(console: Console)\nmake()(console)\nend", 0),
}

SPEC_CASES = {
    "files_folders": ("test \"folders\"\nfiles = Files.in_memory({\"b.txt\": \"x\", \"a/c.txt\": \"\"})\n"
                      "assert files.make_folder(\"n/m\") == Ok(Unit)\n"
                      "assert files.exists?(\"n/m\") and not files.exists?(\"z\")\n"
                      "assert files.entries(\".\") == Ok([Entry(name: \"a\", kind: Folder), "
                      "Entry(name: \"b.txt\", kind: File), Entry(name: \"n\", kind: Folder)])\n"
                      "assert files.entries(\"b.txt\") == "
                      "Error(Failed(path: \"b.txt\", reason: \"not a folder\"))\n"
                      "end\n"
                      "test \"writes\"\nfiles = Files.in_memory({\"b.txt\": \"x\"})\n"
                      "assert files.append_text(\"b.txt\", \"y\") == Ok(Unit)\n"
                      "assert files.read_bytes(\"b.txt\") == Ok(\"xy\".bytes)\n"
                      "assert files.remove(\"b.txt\") == Ok(Unit)\n"
                      "assert files.make_folder(\"d\") == Ok(Unit)\n"
                      "assert files.within(\"d\").write_text(\"e.txt\", \"z\") == Ok(Unit)\n"
                      "reader = files.read_only.within(\"d\")\n"
                      "assert reader.entries(\".\") == Ok([Entry(name: \"e.txt\", kind: File)])\n"
                      "assert reader.exists?(\"e.txt\") and not reader.exists?(\"../d\")\n"
                      "end\n"
                      "test \"entry fields\"\nfiles = Files.in_memory({\"k.txt\": \"\"})\n"
                      "case files.entries(\".\")\n"
                      "Ok(found): assert found.size == 1 and (found.first or Entry(name: \"\", kind: Other)).name == \"k.txt\"\n"
                      "Error(_): assert false\nend\nend\n"
                      "fn main() : Int\n17\nend", {
                          "Main.test.1": ["returned", "Unit"],
                          "Main.test.2": ["returned", "Unit"],
                          "Main.test.3": ["returned", "Unit"],
                      }),
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
    "constant_specs": ("total = twice(seed)\nseed = 7\n"
                       "fn twice(value: T) : T where T: Number, T: Equal\nexamples\n"
                       "twice(seed) == total\nend\npromise \"doubles its input\"\n"
                       "result == value + value\nallows value: 3, result: 6\n"
                       "forbids result: 5, value: 3\nend\nvalue + value\nend\n"
                       "test \"initialized before specs\"\nassert total == 14\nend\n"
                       "fn main() : Int\ntotal\nend", {
                           None: ["returned", "14"],
                           "Main.twice.example.1": ["returned", "Unit"],
                           "Main::Main.twice(Int).promise.1.allows.1": ["returned", "0"],
                           "Main::Main.twice(Int).promise.1.forbids.1": ["returned", "1"],
                           "Main.test.1": ["returned", "Unit"],
                       }),
    "constant_stop": ("unused = crash()\nfn crash() : Int\nstop \"unused initializer\"\nend\n"
                      "test \"not reached\"\nassert false\nend\nfn main() : Int\n17\nend", {
                          None: ["stopped", "unused initializer"],
                          "Main.test.1": ["stopped", "unused initializer"],
                      }),
    "constant_ready_order": ("pending = later + fail_late()\nfirst = fail_first()\nlater = 13\n"
                             "fn fail_late() : Int\nstop \"dependent\"\nend\n"
                             "fn fail_first() : Int\nstop \"ready first\"\nend\n"
                             "fn main() : Int\npending\nend", {
                                 None: ["stopped", "ready first"],
                             }),
    "native_abs_overflow": ("handler = Int.abs\n"
                            "test \"alias overflow\"\nassert handler(-9223372036854775808) == 0\nend\n"
                            "test \"receiver overflow\"\nn = -9223372036854775808\nassert n.abs == 0\nend\n"
                            "fn main() : Int\nInt.abs(-9223372036854775808)\nend", {
                                None: ["stopped", "overflow: Int"],
                                "Main.test.1": ["stopped", "overflow: Int"],
                                "Main.test.2": ["stopped", "overflow: Int"],
                            }),
    "native_abs_initializer_overflow": ("unused = handler(-9223372036854775808)\nhandler = Int.abs\n"
                                        "test \"not reached\"\nassert false\nend\n"
                                        "fn main() : Int\n37\nend", {
                                            None: ["stopped", "overflow: Int"],
                                            "Main.test.1": ["stopped", "overflow: Int"],
                                        }),
    "native_argument_order": ("low = Int.min\nhigh = Int.max\n"
                              "fn first() : Int\nstop \"first argument\"\nend\n"
                              "fn second() : Int\nstop \"second argument\"\nend\n"
                              "test \"alias min order\"\nassert low(first(), second()) == 0\nend\n"
                              "test \"alias max order\"\nassert high(first(), second()) == 0\nend\n"
                              "test \"receiver order\"\nassert first().max(second()) == 0\nend\n"
                              "fn main() : Int\nInt.min(first(), second())\nend", {
                                  None: ["stopped", "first argument"],
                                  "Main.test.1": ["stopped", "first argument"],
                                  "Main.test.2": ["stopped", "first argument"],
                                  "Main.test.3": ["stopped", "first argument"],
                              }),
    "capture_specs": ("fn make(base: T) : fn(T) : fn() : T where T: Number\nexamples\n"
                      "make(13)(29)() == 42\nmake(1.25)(2.5)() == 3.75\nend\n"
                      "fn(extra) fn() base + extra end end\nend\n"
                      "test \"snapshot\"\nvar count = 7\ncopy = count\nshow = fn() copy end\n"
                      "count += 19\nassert show() * 10 + count == 96\nend\n"
                      "test \"captured failure\"\naction = make(13)(29)\nassert action() == 41\nend\n"
                      "fn main() : Int\n17\nend", {
                          None: ["returned", "17"],
                          "Main.make.example.1": ["returned", "Unit"],
                          "Main.make.example.2": ["returned", "Unit"],
                          "Main.test.1": ["returned", "Unit"],
                          "Main.test.2": ["failed", "left 2:42,right 2:41,"],
                      }),
}

REFUSALS = {
    "deferred_lambda": "handler = fn() base end\nbase = 13\nfn main() : Int\nhandler() + 24\nend",
    "deferred_alias": "answer = handler()\nhandler = first\nbase = 13\n"
                      "fn first() : Int\nbase\nend\nfn main() : Int\nanswer + 24\nend",
    "deferred_self_alias": "handler = first\nfn first() : Int\nhandler()\nend\nfn main() : Int\n17\nend",
    "native_pow": "answer = handler(2, 3)\nhandler = Int.pow\nfn main() : Int\nanswer + 30\nend",
}

# These are actual loader inputs, not synthetic symbol tables. Unequal values,
# field layouts and case orders distinguish declaring owners from importers.
MODULE_CASES = {
    "import_constant_read": ({
        "main.mo": "module Main\nuse Lib\nseed = 11\nanswer = Lib.seed * 7 + seed\n"
                   "fn main() : Int\nanswer\nend\n",
        "lib.mo": "module Lib\nexpose seed\nseed = 13\n",
    }, {None: ["returned", "102"]}),
    "import_initializer_routes": ({
        "main.mo": "module Main\nuse Calc{evaluate, score}\n"
                   "direct = evaluate(3)\nhandler = score\nindirect = handler(17)\nseed = 11\n"
                   "lazy = fn() 1 / 0 end\nfn main() : Int\ndirect + indirect + seed\nend\n",
        "calc.mo": "module Calc\nexpose evaluate, score\nseed = 13\n"
                   "fn evaluate(n: Int) : Int\nn + seed * 2\nend\n"
                   "fn score(n: Int) : Int\nn * 7 + 11\nend\n",
    }, {None: ["returned", "170"]}),
    "import_constant_diamond": ({
        "main.mo": "module Main\nuse Left\nuse Right\n"
                   "answer = Left.value * 3 + Right.value + spare\nspare = 5\n"
                   "fn main() : Int\nanswer\nend\n",
        "left.mo": "module Left\nexpose value\nuse Origin\n"
                   "value = Origin.seed * 2 + independent\nindependent = 11\ntag = 17\n",
        "right.mo": "module Right\nexpose value\nuse Origin\n"
                    "value = Origin.seed + Origin.other * 2\n",
        "origin.mo": "module Origin\nexpose seed, other\nseed = 13\nother = 19\n",
    }, {None: ["returned", "167"]}),
    "import_constant_owned": ({
        "main.mo": "module Main\nuse Shapes{Pair, make, snapshot}\n"
                   "packet = make(\"λ\\u{0}owned\", true)\nreader = snapshot(packet)\n"
                   "fn flag(pair: Pair(Text, Bool)) : Bool\npair.second\nend\n"
                   "fn main() : Int\n"
                   "if reader() == \"λ\\u{0}owned\" and flag(packet): 43 else: 1\nend\n",
        "shapes.mo": "module Shapes\nexpose Pair(..), make, snapshot\n"
                     "struct Pair(Z, A)\nfirst: Z\nsecond: A\nend\n"
                     "fn make(value: Z, flag: A) : Pair(Z, A)\n"
                     "Pair(second: flag, first: value)\nend\n"
                     "fn snapshot(pair: Pair(Text, Bool)) : fn() : Text\nfn() pair.first end\nend\n",
    }, {None: ["returned", "43"]}),
    "import_initializer_stop": ({
        "main.mo": "module Main\nuse Fallback\nuse Source\n"
                   "answer = Source.abort()\nlater = Fallback.abort()\n"
                   "test \"not initialized by listing\"\nassert true\nend\n"
                   "fn main() : Int\nstop \"main ran\"\nend\n",
        "source.mo": "module Source\nexpose abort\nfn abort() : Int\n"
                     "stop \"imported initializer\"\nend\n",
        "fallback.mo": "module Fallback\nexpose abort\nfn abort() : Int\n"
                       "stop \"later initializer\"\nend\n",
    }, {None: ["stopped", "imported initializer"],
        "Main.test.1": ["stopped", "imported initializer"]}),
    "routes": ({
        "main.mo": "module Main\nuse Shapes{make, score}\nfn main() : Int\n"
                   "f = score\nf(make(3), 11) + Shapes.score(Shapes.make(5), 2)\nend\n",
        "shapes.mo": "module Shapes\nexpose Box(..), make, score\nstruct Box\nvalue: Int\nend\n"
                     "fn make(value: Int) : Box\nBox(value: value)\nend\n"
                     "fn score(box: Box, extra: Int) : Int\nbox.value * 7 + extra\nend\n",
    }, {None: ["returned", "69"]}),
    "owners": ({
        "main.mo": "module Main\nuse Left\nuse Right\nfn main() : Int\n"
                   "if Left.pick() is Left.One and Right.pick() is Right.Zero\n"
                   "Left.make(3).value * 10 + Right.make(29).other\nelse\n1\nend\nend\n",
        "left.mo": "module Left\nexpose Box(..), Choice(..), make, pick\n"
                   "struct Box\nvalue: Int\nend\nenum Choice\nZero\nOne\nend\n"
                   "fn make(value: Int) : Box\nBox(value: value)\nend\n"
                   "fn pick() : Choice\nOne\nend\n",
        "right.mo": "module Right\nexpose Box(..), Choice(..), make, pick\n"
                    "enum Choice\nOne\nZero\nend\nstruct Box\nother: Int\nend\n"
                    "fn make(value: Int) : Box\nBox(other: value)\nend\n"
                    "fn pick() : Choice\nZero\nend\n",
    }, {None: ["returned", "59"]}),
    "transitive": ({
        "main.mo": "module Main\nuse Relay\nfn main() : Int\nRelay.forward(17).value\nend\n",
        "relay.mo": "module Relay\nexpose forward\nuse Origin{Box, build}\n"
                    "fn forward(value: Int) : Box\nbuild(value + 6)\nend\n",
        "origin.mo": "module Origin\nexpose Box(..), build\nstruct Box\nvalue: Int\nend\n"
                     "fn build(value: Int) : Box\nBox(value: value)\nend\n",
    }, {None: ["returned", "23"]}),
    "snapshot": ({
        "main.mo": "module Main\nuse Store{Box, make, read}\n"
                   "fn snapshot(box: Box) : fn() : Int\nfn() read(box) end\nend\n"
                   "fn main() : Int\nvar box = make(3)\naction = snapshot(box)\n"
                   "box.value += 19\naction() * 10 + box.value\nend\n",
        "store.mo": "module Store\nexpose Box(..), make, read\nstruct Box\nvalue: Int\nend\n"
                    "fn make(value: Int) : Box\nBox(value: value)\nend\n"
                    "fn read(box: Box) : Int\nbox.value\nend\n",
    }, {None: ["returned", "52"]}),
    "opaque": ({
        "main.mo": "module Main\nuse Store{Token, open, read}\n"
                   "fn keep(token: Token) : Token\ntoken\nend\n"
                   "fn main() : Int\nread(keep(open(37)))\nend\n",
        "store.mo": "module Store\nexpose Token, open, read\nstruct Token\nvalue: Int\nend\n"
                    "fn open(value: Int) : Token\nToken(value: value)\nend\n"
                    "fn read(token: Token) : Int\ntoken.value\nend\n",
    }, {None: ["returned", "37"]}),
    "specs": ({
        "main.mo": "module Main\nuse Math{score}\ntest \"imported failure\"\n"
                   "assert score(3, 11) == 33\nend\nfn main() : Int\nscore(3, 11)\nend\n",
        "math.mo": "module Math\nexpose score\nfn score(left: Int, right: Int) : Int\n"
                   "examples\nscore(3, 11) == 32\nend\nleft * 7 + right\nend\n",
    }, {None: ["returned", "32"], "Math.score.example.1": ["returned", "Unit"],
        "Main.test.1": ["failed", "left 2:32,right 2:33,"]}),
    "generic_keys": ({
        "main.mo": "module Main\nuse Calc\n"
                   "fn relay(value: A, tag: Z) : A where A: Number, Z: Text\n"
                   "f = Calc.choose\nf(value, tag)\nend\n"
                   "fn main() : Int\nrelay(13, true) * 3 + relay(17, false)\nend\n",
        "calc.mo": "module Calc\nexpose choose\n"
                   "fn choose(value: Z, tag: A) : Z where Z: Number, A: Text\n"
                   "if \"#{tag}\" == \"true\": value + value else: value\nend\n",
    }, {None: ["returned", "95"]}),
    "generic_equal_actuals": ({
        "main.mo": "module Main\nuse Calc{pair}\nfn main() : Int\n"
                   "f = pair\n(a, b) = f(3, 17)\n(c, d) = pair(1.25, 19)\n"
                   "if c == 2.5 and d == 57: a * 10 + b else: 1\nend\n",
        "calc.mo": "module Calc\nexpose pair\n"
                   "fn pair(left: Z, right: A) : (Z, A) where Z: Number, A: Number\n"
                   "(left + left, right + right + right)\nend\n",
    }, {None: ["returned", "111"]}),
    "generic_proofs": ({
        "main.mo": "module Main\nuse Calc\nstruct Pair(Z, A)\nfirst: Z\nsecond: A\nend\n"
                   "fn to_text(pair: Pair(U, T)) : Text where U: Text, T: Text\n"
                   "\"[#{pair.first}|#{pair.second}]\"\nend\nstruct Box\nvalue: Int\nend\n"
                   "fn compare(a: Box, b: Box) : Ordering\n"
                   "if a.value < b.value: Greater else: Less\nend\n"
                   "fn main() : Int\np = Pair(first: true, second: 19)\n"
                   "if Calc.show(p) == \"[true|19]\" "
                   "and Calc.show(Pair(first: p, second: false)) == \"[[true|19]|false]\" "
                   "and Calc.show([p]) == \"[[true|19]]\" "
                   "and Calc.keep([3, 17]) == [3, 17] "
                   "and Calc.ordered(Box(value: 3), Box(value: 17)) == false: 47 else: 1\nend\n",
        "calc.mo": "module Calc\nexpose show, keep, ordered\n"
                   "fn show(value: T) : Text where T: Text\n\"#{value}\"\nend\n"
                   "fn keep(value: T) : T where T: Hash\n"
                   "if value == value: value else: value\nend\n"
                   "fn ordered(a: T, b: T) : Bool where T: Order\na < b\nend\n",
    }, {None: ["returned", "47"]}),
    "generic_escaped": ({
        "main.mo": "module Main\nuse Calc\nuse Bridge{relay, concrete}\n"
                   "fn wrap(f: fn(Int) : fn(Int) : Int, base: Int) : fn(Int) : Int\n"
                   "fn(extra) f(base)(extra) end\nend\nfn main() : Int\n"
                   "factory = Calc.offset\na = relay(3)\nb = concrete(5)\nc = wrap(factory, 17)\n"
                   "if Calc.offset(1.25)(2.5) == 3.75: a(11) + b(7) + c(29) else: 1\nend\n",
        "calc.mo": "module Calc\nexpose offset\n"
                   "fn offset(value: T) : fn(T) : T where T: Number\n"
                   "fn(extra) value + extra end\nend\n",
        "bridge.mo": "module Bridge\nexpose relay, concrete\nuse Calc{offset}\n"
                     "fn relay(value: U) : fn(U) : U where U: Number\nf = offset\nf(value)\nend\n"
                     "fn concrete(value: Int) : fn(Int) : Int\noffset(value)\nend\n",
    }, {None: ["returned", "72"]}),
    "generic_specs": ({
        "main.mo": "module Main\nuse Calc\ntest \"imported generic failure\"\n"
                   "assert Calc.twice(3) == 7\nend\nfn main() : Int\n17\nend\n",
        "calc.mo": "module Calc\nexpose twice\n"
                   "fn twice(value: T) : T where T: Number\n"
                   "examples\ntwice(3) == 6\nend\nvalue + value\nend\n",
    }, {None: ["returned", "17"], "Calc.twice.example.1": ["returned", "Unit"],
        "Main.test.1": ["failed", "left 1:6,right 1:7,"]}),
    "generic_scalars": ({
        "main.mo": "module Main\nuse Calc\nfn probe(big: BigInt, float: Float) : Int\n"
                   "if Calc.twice(big) == 36893488147419103232 and Calc.twice(float) == -3.0 "
                   "and Calc.twice(1.25) == 2.5: 83 else: 1\nend\n"
                   "fn main() : Int\nprobe(0x10000000000000000, -1.5)\nend\n",
        "calc.mo": "module Calc\nexpose twice\n"
                   "fn twice(value: T) : T where T: Number\nvalue + value\nend\n",
    }, {None: ["returned", "83"]}),
    "nominal_written": ({
        "main.mo": "module Main\nuse Shapes{Pair}\n"
                   "fn score(pair: Pair(Int, Bool)) : Int\n"
                   "if pair.second: pair.first * 10 + 7 else: pair.first - 3\nend\n"
                   "fn main() : Int\nscore(Pair(second: true, first: 3))\nend\n",
        "shapes.mo": "module Shapes\nexpose Pair(..)\nstruct Pair(Z, A)\n"
                     "first: Z\nsecond: A\nend\n",
    }, {None: ["returned", "37"]}),
    "nominal_owners": ({
        "main.mo": "module Main\nuse Left\nuse Right\n"
                   "fn left(value: Left.Choice(Int, Bool)) : Int\ncase value\n"
                   "Left.Value(pair: p): if p.second: p.first * 10 + 7 else: p.first - 3\n"
                   "Left.Empty: 1\nend\nend\n"
                   "fn right(value: Right.Choice(Int, Bool)) : Int\ncase value\n"
                   "Right.Empty: 2\nRight.Value(p): if p.first: p.second * 2 + 5 else: 3\nend\nend\n"
                   "fn main() : Int\n"
                   "left(Left.Value(pair: Left.Pair(second: true, first: 3))) + "
                   "right(Right.Value(Right.Pair(second: 17, first: true))) + "
                   "left(Left.Empty) + right(Right.Empty)\nend\n",
        "left.mo": "module Left\nexpose Pair(..), Choice(..)\n"
                   "struct Pair(Z, A)\nfirst: Z\nsecond: A\nend\n"
                   "enum Choice(Z, A)\nEmpty\nValue(pair: Pair(Z, A))\nend\n",
        "right.mo": "module Right\nexpose Pair(..), Choice(..)\n"
                    "enum Choice(Z, A)\nValue(Pair(Z, A))\nEmpty\nend\n"
                    "struct Pair(Z, A)\nfirst: A\nsecond: Z\nend\n",
    }, {None: ["returned", "79"]}),
    "nominal_instances": ({
        "main.mo": "module Main\nuse Shapes{Pair}\nuse Bridge\n"
                   "fn relay(pair: Pair(A, Z)) : Pair(A, Z) where A: Number\n"
                   "Shapes.bump(pair)\nend\nfn main() : Int\n"
                   "a = relay(Pair(second: true, first: 3))\n"
                   "b = Shapes.bump(Pair(first: 7, second: 11))\n"
                   "c = Shapes.bump(Pair(first: 1.25, second: 19))\n"
                   "d = Bridge.entry()\ne = Shapes.bump(Shapes.bump(Pair(first: 3, second: true)))\n"
                   "if c.first == 2.5 and c.second == 19 and a.second and not d.second\n"
                   "a.first * 10 + b.first + b.second + d.first + e.first\nelse\n1\nend\nend\n",
        "shapes.mo": "module Shapes\nexpose Pair(..), bump\nstruct Pair(Z, A)\n"
                     "first: Z\nsecond: A\nend\n"
                     "fn bump(pair: Pair(Z, A)) : Pair(Z, A) where Z: Number\n"
                     "examples\nbump(Pair(first: 3, second: true)).first == 6\nend\n"
                     "Pair(second: pair.second, first: pair.first + pair.first)\nend\n",
        "bridge.mo": "module Bridge\nexpose entry\nuse Shapes{Pair, bump}\n"
                     "fn relay(pair: Pair(T, U)) : Pair(T, U) where T: Number\nbump(pair)\nend\n"
                     "fn entry() : Pair(Int, Bool)\nrelay(Pair(first: 5, second: false))\nend\n",
    }, {None: ["returned", "107"], "Shapes.bump.example.1": ["returned", "Unit"]}),
    "nominal_opaque": ({
        "main.mo": "module Main\nuse Store{Token, read}\nuse Bridge{forward}\n"
                   "fn keep(token: Token(Int)) : Token(Int)\ntoken\nend\n"
                   "fn main() : Int\nread(keep(forward(43)))\nend\n",
        "bridge.mo": "module Bridge\nexpose forward\nuse Store{Token, open}\n"
                     "fn forward(value: Int) : Token(Int)\nopen(value)\nend\n",
        "store.mo": "module Store\nexpose Token, open, read\nstruct Token(T)\nvalue: T\nend\n"
                    "fn open(value: T) : Token(T)\nToken(value: value)\nend\n"
                    "fn read(token: Token(T)) : T\ntoken.value\nend\n",
    }, {None: ["returned", "43"]}),
}


def check_modules(driver, vm, root, world):
    checked = 0
    for name, (files, selections) in MODULE_CASES.items():
        folder = root / name
        folder.mkdir()
        for filename, text in files.items():
            (folder / filename).write_text(text)
        result = subprocess.run([str(driver), str(folder / "main.mo")], capture_output=True,
                                text=True, timeout=30)
        assert result.returncode == 0, (name, result.stderr)
        path = folder / "lower.core"
        path.write_text(result.stdout)
        program = parse(result.stdout)[2]
        metadata = program[-1]
        assert metadata[0] == "Metadata", name
        originals = [pathlib.Path(p).read_bytes() for p in metadata[1]]
        assert {pathlib.Path(p).name for p in metadata[1]} == set(files), name
        for module in metadata[2]:
            for index in module[2]:
                assert program[1][int(index)][1].startswith(module[1] + "."), (name, module)
            constant_starts = []
            for index in module[4]:
                declaration, meta = program[3][int(index)], metadata[4][int(index)]
                assert declaration[1] == meta[1] and meta[1].startswith(module[1] + "."), (
                    name, module, meta)
                span = meta[2][1]
                original = originals[int(span[1])]
                assert original.split(b"\n", 1)[0].decode() == "module " + module[1], (
                    name, module, meta)
                start, end = int(span[2]), int(span[3])
                assert 0 <= start < end <= len(original), (name, meta)
                assert original[start:end].startswith(meta[1].rsplit(".", 1)[1].encode() + b" ="), (
                    name, meta)
                constant_starts.append(start)
                for optional in meta[3][1]:
                    if optional[0] == "some":
                        node = optional[1]
                        assert node[1] == span[1], (name, meta, node)
                        assert 0 <= int(node[2]) <= int(node[3]) <= len(original), (name, node)
                assert all(closure[1].startswith(meta[1] + ".") for closure in meta[3][4]), (
                    name, meta)
            assert constant_starts == sorted(constant_starts), (name, module, constant_starts)
            for index in module[3]:
                fun, meta = program[2][int(index)], metadata[3][int(index)]
                assert fun[5] == module[1], (name, module, fun)
                if fun[1].startswith("%"):
                    continue
                span = meta[2][1]
                original = originals[int(span[1])]
                declaring = original.split(b"\n", 1)[0].decode().removeprefix("module ")
                generated = ".%" in fun[1]
                prefix = (module[1] + "::" + declaring if "::" in fun[1] else module[1])
                assert fun[1].startswith(prefix + "."), (name, module, meta)
                start, end = int(span[2]), int(span[3])
                assert 0 <= start < end <= len(original), (name, meta)
                header = original[start:end].split(b"\n", 1)[0]
                if fun[6][0] == "example":
                    assert meta[3] == ["some", original[start:end].decode()], (name, meta)
                elif not generated:
                    assert header.startswith((b"fn ", b"test ")), (name, header)
                for optional in meta[4][1]:
                    if optional[0] == "some":
                        node = optional[1]
                        assert node[1] == span[1], (name, meta, node)
                        assert 0 <= int(node[2]) <= int(node[3]) <= len(original), (name, node)
            exposed = [program[2][int(i)][1] for i in module[6]]
            assert all(n.startswith((module[1] + ".", module[1] + "::" + module[1] + "."))
                       for n in exposed), (name, exposed)
        if name == "import_constant_diamond":
            assert [constant[1] for constant in program[3]] == [
                "Main.spare", "Left.independent", "Left.tag", "Origin.seed", "Left.value",
                "Origin.other", "Right.value", "Main.answer"], program[3]
        if name == "import_initializer_stop":
            assert [constant[1] for constant in program[3]] == ["Main.answer", "Main.later"], program[3]
        if name == "import_constant_owned":
            assert [data[1] for data in program[1]] == ["Shapes.Pair(Text, Bool)"], program[1]
            main = next(module for module in metadata[2] if module[1] == "Main")
            shapes = next(module for module in metadata[2] if module[1] == "Shapes")
            assert not main[2] and not main[5] and shapes[5] == [["0", "true"]], metadata[2]
            snapshot = next(meta for meta in metadata[3] if meta[1] == "Shapes.snapshot")
            assert [closure[1] for closure in snapshot[4][4]] == ["Shapes.snapshot.fn.1"], snapshot
        if name == "owners":
            assert {data[1] for data in program[1]} == {
                "Left.Box", "Right.Box", "Left.Choice", "Right.Choice"}, program[1]
        if name == "transitive":
            assert [data[1] for data in program[1]] == ["Origin.Box"], program[1]
        if name == "opaque":
            store = next(module for module in metadata[2] if module[1] == "Store")
            assert store[5] == [["0", "false"]], store
        if name == "generic_escaped":
            names = [fun[1] for fun in program[2]]
            for instance in ("Main::Calc.offset(Int)", "Bridge::Calc.offset(Int)",
                             "Main::Calc.offset(Decimal)", "Main::Bridge.relay(Int)"):
                assert names.count(instance) == 1, (name, instance, names)
        if name == "nominal_written":
            assert [data[1] for data in program[1]] == ["Shapes.Pair(Int, Bool)"], program[1]
            assert program[1][0][2] == [["Ctor", "Pair", [["first", ["int"]],
                                                        ["second", ["bool"]]]]], program[1]
        if name == "nominal_owners":
            assert {data[1] for data in program[1]} == {
                "Left.Pair(Int, Bool)", "Left.Choice(Int, Bool)",
                "Right.Pair(Int, Bool)", "Right.Choice(Int, Bool)"}, program[1]
        if name == "nominal_instances":
            assert {data[1] for data in program[1]} == {
                "Shapes.Pair(Int, Bool)", "Shapes.Pair(Int, Int)",
                "Shapes.Pair(Decimal, Int)"}, program[1]
            names = [fun[1] for fun in program[2]]
            for instance in ("Main::Shapes.bump(Bool, Int)", "Bridge::Shapes.bump(Bool, Int)"):
                assert names.count(instance) == 1, (name, instance, names)
            assert sum(fun[6][0] == "example" for fun in program[2]) == 1, (name, names)
        if name == "nominal_opaque":
            assert [data[1] for data in program[1]] == ["Store.Token(Int)"], program[1]
            store = next(module for module in metadata[2] if module[1] == "Store")
            assert store[5] == [["0", "false"]], store
        for target, expected in selections.items():
            assert oracle(path, world, target) == expected, (name, target)
            runner = vm if target is None else vm.parent / "tests/test_vm.test"
            assert run(str(runner), path, target) == expected, (name, target)
            checked += 1
    print(f"lower modules: {len(MODULE_CASES)} loaded programs; {checked} expected outcomes "
          "match Lean/original-document VM; declaring owners, exports and spans retained")


for fixture, expected in {
    "file-error-text": 0,
    "char-natives": 0,
    "list-all": 0,
    "list-count": 0,
    "list-find": 0,
    "list-max": 0,
    "list-zip-entries": 0,
    "list-map": 0,
    "option-or-fail": 0,
    "option-result-map": 0,
    "json": 0,
    "map-update-sort-desc": 0,
    "map-literals": 0,
    "map-evaluation-order": 0,
    "map-captures": 0,
    "map-native-routes": 0,
    "map-mutating-key": 0,
    "map-runtime-duplicate": ["stopped", "two keys of a map literal that turn out equal"],
    "set": 0,
    "time-sub": 0,
    "closure-captures": 0,
    "closure-returned": 0,
    "generic-higher-order": 0,
    "generic-case": 0,
    "generic-subject": 0,
}.items():
    body = (pathlib.Path("tests/lower-source") / fixture / "main.mo").read_text()
    assert body.startswith("module Main\n")
    CASES[fixture] = (body[len("module Main\n"):], expected)


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
        check_modules(driver, vm, root, world)
        for name, (body, status) in CASES.items():
            source = root / "main.mo"
            source.write_text("module Main\n" + body + "\n")
            result = subprocess.run([str(driver), str(source), LIBRARY], capture_output=True,
                                    text=True, timeout=30)
            assert result.returncode == 0, (name, result.returncode, result.stderr)
            path = root / f"{name}.core"
            path.write_text(result.stdout)
            lengths[name] = len(result.stdout.encode())
            metadata = parse(result.stdout)[2][-1]
            assert metadata[0] == "Metadata"
            names = [closure[1] for fun in metadata[3] for closure in fun[4][4]]
            assert len(names) == len(set(names)), (name, "duplicate closure display names", names)
            expected = ["returned", str(status)] if isinstance(status, int) else status
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
        for name, body in REFUSALS.items():
            source = root / "main.mo"
            source.write_text("module Main\n" + body + "\n")
            result = subprocess.run([str(driver), str(source), LIBRARY], capture_output=True,
                                    text=True, timeout=30)
            assert result.returncode == 1 and result.stdout == "" and result.stderr.startswith("E1150:"), (name, result)
        print(f"lower source: {len(REFUSALS)} explicit deferred-global/native refusals")
        for path in sorted(pathlib.Path("examples").glob("*.mo")):
            result = subprocess.run([str(driver), str(path.resolve()), LIBRARY], capture_output=True,
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
