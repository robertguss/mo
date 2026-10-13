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

from check_core_check import VERSION, WORLD, parse
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
