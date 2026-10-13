#!/usr/bin/env python3
"""Source/check/lower integration, after linking ci/lower_source.c with mo_check.

Run from the repository root: python3 ci/check_lower_source.py LOWER_SOURCE
TEST_VM [--require-vm]. This is separate from main's contract-fixture gate until
the staged checker merges. The twelve shipped examples are also probed; staged
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
    "lambda": ("enum Choice\nFirst(Int)\nSecond(Int)\nEmpty\nend\n"
               "fn main() : Int\nf = case First(7)\n"
               "First(_) | Second(_): fn(x)\nx + 1\nend\n"
               "Empty: fn(x)\nx + 2\nend\nend\nf(4)\nend", 5),
}


def main():
    driver, vm = map(pathlib.Path, sys.argv[1:3])
    require_vm = "--require-vm" in sys.argv
    matched = refused = 0
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
            expected = ["returned", str(status)]
            actual = oracle(path, world)
            assert actual == expected, (name, expected, actual)
            actual = run(str(vm), path)
            if actual is None:
                refused += 1
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
    print(f"lower source: {len(CASES)} expected outcomes match Lean; "
          f"{matched} VM outcomes match; {refused} compiler/host refusals")


if __name__ == "__main__":
    main()
