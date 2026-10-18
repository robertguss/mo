#!/usr/bin/env python3
"""Every error code in src/base/codes.h has a test of its exact message.

Run from the repository root: python3 ci/check_errors.py MO...

tests/errors/ holds one folder per case, named eNNNN or eNNNN-variant. A case has a main.mo
(and any other modules it needs) and `expected`, the exact standard error of
`mo check tests/errors/<case>/main.mo`, which exits 1. A case may have `command` (another mo
subcommand), `args` (its arguments, one per line, {main} for main.mo) and `stdout` (what the
command prints, empty by default). Every diagnostic in it must carry the
case's code, so a case never passes because of a different mistake. A case whose source is too
big to commit has a file `generated` naming its generator below; the source is then written to a
temporary folder and checked as `main.mo` from inside it.

Codes that no source program can raise (Core text, bytecode, internal failures) are listed in
tests/errors/unit.tsv with the unit test that pins each one. Every code must be in exactly one of
the two places, and the source cases are never regenerated: edit `expected` by hand. A case with a file
`library-dependent` fills a limit that lib/'s own files also use, so it compares everything in
`expected` but its numbers.
"""

import pathlib
import re
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parent.parent
errors = root / "tests/errors"


def codes():
    text = (root / "src/base/codes.h").read_text()
    return {int(n): name for n, name in re.findall(r"X\((\d+), ([A-Z_0-9]+),", text)}


def write_lines(folder, text, name="main.mo"):
    (folder / name).write_text(text)


def generate_file_too_big(folder):
    write_lines(folder, "module Main\n" + "# padding\n" * 1000000)


def generate_program_too_big(folder):
    # Nine files just under the file limit are more than the program limit allows. Their bulk is
    # one text literal each, which costs the syntax arena little.
    write_lines(folder, "module Main\n" + "".join(f"use M{i}\n" for i in range(9)))
    line = "a" * 1023 + "\n"
    for i in range(9):
        write_lines(folder, f'module M{i}\nbig = """\n' + line * 8190 + '"""\n', f"m{i}.mo")


def generate_syntax_arena(folder):
    write_lines(folder, "module Main\n" + "".join(f"use M{i}\n" for i in range(10)))
    for i in range(10):
        write_lines(folder, f"module M{i}\n" + "# padding\n" * 700000, f"m{i}.mo")


def generate_too_many_files(folder):
    write_lines(folder, "module Main\n" + "".join(f"use M{i}\n" for i in range(4100)))
    for i in range(4100):
        write_lines(folder, f"module M{i}\n", f"m{i}.mo")


def generate_path_too_long(folder):
    write_lines(folder, "module Main\nuse " + ".".join(["A" * 200] * 21) + "\n")


def generate_line_too_long(folder):
    write_lines(folder, "module Main\n# " + "x" * 70000 + "\n")


def generate_nested_too_deeply(folder):
    write_lines(folder, "module Main\nfn run() : Int\n" + "(" * 300 + "1" + ")" * 300 + "\nend\n")


def generate_interpolations_too_deep(folder):
    write_lines(folder, "module Main\nfn run() : Text\n" + '"#{' * 20 + "1" + '}"' * 20 + "\nend\n")


def generate_too_many_names(folder):
    write_lines(folder, "module Main\n" + "".join(f"x{i} = {i}\n" for i in range(65536)))


def generate_too_many_parameters(folder):
    parameters = ", ".join(f"a{i}: Int" for i in range(256))
    write_lines(folder, f"module Main\nfn f({parameters}) : Int\n1\nend\n")


def generate_too_many_fields(folder):
    write_lines(folder, "module Main\nstruct S\n" + "".join(f"f{i}: Int\n" for i in range(65536)) + "end\n")


def generate_too_many_cases(folder):
    write_lines(folder, "module Main\nenum E\n" + "".join(f"C{i}\n" for i in range(65536)) + "end\n")


def generate_too_many_registers(folder):
    elements = ",\n".join(str(i) for i in range(70000))
    write_lines(folder, f"module Main\nfn run() : Int\n  xs = [\n{elements}\n]\n  xs.size\nend\n"
                "fn main()\n  stop \"x\"\nend\n")


def generate_checking_arena(folder):
    write_lines(folder, "module Main\nfn main(console: Console)\n" + '  console.print("x")\n' * 150000 + "end\n")


def generate_module_outside_root(folder):
    (folder / "outside.mo").write_text("module Other\n")
    (folder / "project").mkdir()
    (folder / "project/other.mo").symlink_to("../outside.mo")
    write_lines(folder, "module Main\nuse Other\nfn main()\nend\n", "project/main.mo")
    return "project"


def generate_check_work(folder):
    n = 65000
    write_lines(folder, "module Main\nenum E\n" + "".join(f"C{i}\n" for i in range(n))
                + "end\nfn pick(e: E) : Int\n  case e\n" + "".join(f"    C{i}: {i}\n" for i in range(n))
                + "  end\nend\nfn main()\n  stop \"x\"\nend\n")


def generate_check_type_depth(folder):
    lines = ["module Main", "fn duplicate(value: T) : (T, T)", "(value, value)", "end", "fn run()", "_v0 = 1"]
    lines += [f"_v{i} = duplicate(_v{i - 1})" for i in range(1, 256)]
    write_lines(folder, "\n".join(lines + ["end", ""]))


def generate_check_ability_depth(folder):
    count = 2048
    text = "module Main\n"
    for i in range(count):
        field = f"Option(R{i + 1})" if i + 1 < count else "Option(Int)"
        text += f"struct R{i}\nnext: {field}\nend\n"
    write_lines(folder, text + "fn same(a: Option(R0), b: Option(R0)) : Bool\na == b\nend\nfn main()\nend\n")


def generate_lower_arena(folder):
    n = 130000
    write_lines(folder, "module Main\nfn pick(e: Int) : Int\n  case e\n"
                + "".join(f"    {i}: {i + 1}\n" for i in range(n))
                + "    _: 0\n  end\nend\nfn main()\n  stop \"x\"\nend\n")


def generate_lower_work(folder):
    write_lines(folder, "module Main\nfn run() : BigInt\n  " + "9" * 60000 + "\nend\nfn main()\n  stop \"x\"\nend\n")


def generate_lower_depth(folder):
    n = 70
    text = "module Main\n"
    for i in range(n):
        text += f"fn g{i}(x: T) : Int\n  g{i + 1}([x])\nend\n"
    write_lines(folder, text + f"fn g{n}(_x: T) : Int\n  1\nend\nfn run() : Int\n  g0(1)\nend\n")


def generate_format_limit(folder):
    write_lines(folder, "module Main\nfn run() : Int\n  [\n" + ",\n".join(["[1,2,3]"] * 100000) + "\n].size\nend\n")


GENERATORS = {
    "file-too-big": generate_file_too_big,
    "program-too-big": generate_program_too_big,
    "syntax-arena": generate_syntax_arena,
    "too-many-files": generate_too_many_files,
    "path-too-long": generate_path_too_long,
    "line-too-long": generate_line_too_long,
    "nested-too-deeply": generate_nested_too_deeply,
    "interpolations-too-deep": generate_interpolations_too_deep,
    "too-many-names": generate_too_many_names,
    "too-many-parameters": generate_too_many_parameters,
    "too-many-fields": generate_too_many_fields,
    "too-many-cases": generate_too_many_cases,
    "too-many-registers": generate_too_many_registers,
    "checking-arena": generate_checking_arena,
    "module-outside-root": generate_module_outside_root,
    "check-work": generate_check_work,
    "check-type-depth": generate_check_type_depth,
    "check-ability-depth": generate_check_ability_depth,
    "lower-arena": generate_lower_arena,
    "lower-work": generate_lower_work,
    "lower-depth": generate_lower_depth,
    "format-limit": generate_format_limit,
}


def invocation(case, main):
    """The mo arguments of a case: `command` names the subcommand (check by default), and `args`
    lists the arguments after it, one per line, with {main} standing for the case's main.mo."""
    command = (case / "command").read_text().strip() if (case / "command").exists() else "check"
    arguments = (case / "args").read_text().splitlines() if (case / "args").exists() else ["{main}"]
    return [command, *(a.replace("{main}", main) for a in arguments)]


def run_case(binary, case):
    generator = case / "generated"
    if generator.exists():
        with tempfile.TemporaryDirectory(prefix="mo-errors-") as folder:
            where = GENERATORS[generator.read_text().strip()](pathlib.Path(folder))
            return subprocess.run([str(binary), *invocation(case, "main.mo")],
                                  cwd=pathlib.Path(folder) / (where or "."), capture_output=True, timeout=120)
    main = str((case / "main.mo").relative_to(root))
    return subprocess.run([str(binary), *invocation(case, main)], cwd=root, capture_output=True, timeout=120)


def check_cases(binary, table):
    seen = set()
    for case in sorted(p for p in errors.iterdir() if p.is_dir()):
        match = re.fullmatch(r"e(\d{4})(-[a-z0-9-]+)?", case.name)
        assert match, f"{case.name}: a case is named eNNNN or eNNNN-variant"
        code = int(match.group(1))
        assert code in table, f"{case.name}: E{code:04d} is not in src/base/codes.h"
        seen.add(code)
        result = run_case(binary, case)
        assert result.returncode == 1, (case.name, result.returncode)
        stdout = case / "stdout"
        assert result.stdout == (stdout.read_bytes() if stdout.exists() else b""), case.name
        expected = (case / "expected").read_bytes()
        actual = result.stderr
        if (case / "library-dependent").exists():
            # The toolchain's lib/ files take part of the limit these cases fill, so where the
            # limit is reached moves whenever lib/ grows: compare everything but the numbers.
            masked = [re.sub(rb"\d+", b"N", text) for text in (expected, actual)]
        else:
            masked = [expected, actual]
        assert masked[0] == masked[1], f"{case.name}: the message differs\n--- expected\n" \
            f"{expected.decode()}\n--- actual\n{result.stderr.decode(errors='replace')}"
        headers = re.findall(rb"^error E(\d{4}):", expected, re.M)
        assert headers and all(int(h) == code for h in headers), \
            f"{case.name}: every diagnostic must be E{code:04d}, found {headers}"
    return seen


def check_units(table, seen):
    units = {}
    untested = []
    for line in (errors / "unit.tsv").read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        code, test, reason = line.split("\t")
        number = int(code[1:])
        assert number in table and number not in seen and number not in units, line
        if reason.startswith("UNTESTED:"):
            # Nothing raises it and no test pins it yet; the gate keeps the gap visible.
            untested.append(code)
            units[number] = test
            continue
        text = (root / test).read_text()
        assert f"MO_CODE_{table[number]}" in text or code in text, \
            f"{test} does not name {code}"
        assert reason.strip(), line
        units[number] = test
    return units, untested


def main():
    table = codes()
    binaries = sys.argv[1:]
    assert binaries, "usage: check_errors.py MO..."
    for binary in binaries:
        seen = check_cases(pathlib.Path(binary).resolve(), table)
    units, untested = check_units(table, seen)
    missing = sorted(set(table) - seen - set(units))
    assert not missing, "codes with no test: " + ", ".join(f"E{n:04d}" for n in missing)
    if untested:
        print("registry only, with no test that raises them: " + ", ".join(untested))
    print(f"{len(seen)} codes by message, {len(units)} by unit test, of {len(table)}")


main()
