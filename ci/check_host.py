#!/usr/bin/env python3
"""Compare persistent fake handles with mocore's operations and state.

Constructors run in spec mode and expose their new CapState through effects.
Other operations run with that fake handle injected into a checked Run. Console
observations use the model's recorded lines (rendered by mocore's identity
function), since spec-only observations cannot return non-Unit from a test.
"""

import pathlib
import random
import re
import subprocess
import sys
import tempfile

from check_core_check import VERSION, WORLD, parse, wire


ROWS = re.findall(r'^HOSTOP\((\w+),', pathlib.Path("src/base/hostops.def").read_text(), re.M)
IDS = {name: i for i, name in enumerate(ROWS)}
OPS = {
    "ARGS_LIST": ("argsList", "(list (text))"),
    "ENV_GET": ("envGet", "(option (text))"),
    "CONSOLE_PRINT": ("print", "(unit)"),
    "CONSOLE_ERROR": ("printError", "(unit)"),
    "CONSOLE_READ_LINE": ("readLine", "(option (text))"),
    "CLOCK_NOW": ("now", "(time)"),
    "RANDOM_INT": ("randomInt", "(int)"),
    "FILES_WRITE_TEXT": ("writeText", "(result (unit) (fileError))"),
    "FILES_APPEND_TEXT": ("appendText", "(result (unit) (fileError))"),
    "FILES_REMOVE": ("remove", "(result (unit) (fileError))"),
    "FILES_MAKE_FOLDER": ("makeFolder", "(result (unit) (fileError))"),
    "FILES_READ_ONLY": ("readOnly", "(cap (readFiles))"),
}
for prefix, ro in [("FILES", "false"), ("READFILES", "true")]:
    for name, io, ty in [
        ("READ_TEXT", "readText", "(result (text) (fileError))"),
        ("READ_BYTES", "readBytes", "(result (bytes) (fileError))"),
        ("LIST", "list", "(result (list (text)) (fileError))"),
        ("ENTRIES", "entries", "(result (list (entry)) (fileError))"),
        ("EXISTS", "exists", "(bool)"),
        ("WITHIN", "within", f"(cap ({'files' if ro == 'false' else 'readFiles'}))")]:
        OPS[f"{prefix}_{name}"] = (f"{io} {ro}", ty)
CONSTRUCTORS = {
    "ARGS_OF": ("argsOf", "args"), "ENV_OF": ("envOf", "env"),
    "CONSOLE_FAKE": ("consoleFake", "console"),
    "CONSOLE_FAKE_INPUT": ("consoleFakeInput", "console"),
    "FILES_IN_MEMORY": ("filesInMemory", "files"),
    "CLOCK_FIXED": ("clockFixed", "clock"), "RANDOM_SEEDED": ("randomSeeded", "random"),
}
OBSERVATIONS = {"CONSOLE_PRINTED": 4, "CONSOLE_PRINTED_ERRORS": 5}
assert set(ROWS) == set(OPS) | set(CONSTRUCTORS) | set(OBSERVATIONS)


def text(s):
    return f"(lit (text {wire(s)}))"


def oracle(root, program, world, selection):
    path = root / "operation.core"
    path.write_text(f"(core {VERSION} (Run {program} {wire(world)} {selection} 10000))\n")
    result = subprocess.run(["formal/.lake/build/bin/mocore", str(path)],
                            capture_output=True, text=True, timeout=15)
    assert result.returncode in (0, 1), (result.returncode, path.read_text(), result.stdout, result.stderr)
    report = parse(result.stdout)[2]
    effects = report[2]
    # The model reports these fields even for a stop; retain every fake state.
    world[4], world[5], world[6], world[9], world[10] = effects[1:]
    return report[:2]


def expected(root, commands):
    world, handles, outcomes = parse(WORLD), {}, []
    for name, source, destination, path, value, low, high in commands:
        if name in CONSTRUCTORS:
            io, cap = CONSTRUCTORS[name]
            args = ""
            if name in ("ARGS_OF", "CONSOLE_FAKE_INPUT"):
                args = f"(prim (list (text) (lit 2)) ({text(path)} {text(value)}))"
            elif name in ("FILES_IN_MEMORY", "ENV_OF"):
                pairs = [] if low == 0 else [text(path), text(value)]
                if low == 2:
                    pairs += [text(value), text(path)]
                args = f"(prim (map (text) (text) (lit {low})) ({' '.join(pairs)}))"
            elif name in ("CLOCK_FIXED", "RANDOM_SEEDED"):
                args = f"(lit ({'time' if name == 'CLOCK_FIXED' else 'int'} {low}))"
            body = f'(let_ "new" (prim (io ({io})) ({args})) (lit (unit)))'
            program = f'''(Program () (
                (FunDecl "entry" () (unit) (lit (unit)) "M" (function) 0)
                (FunDecl "test" () (unit) {body} "M" (test) 0)) () "entry" (noSource))'''
            previous_count = len(world[10])
            got = oracle(root, program, world, '(test "test")')
            if got[0] == "stopped":
                assert got[1][0] == "fileFolderConflict", got
                outcomes.append(["stopped", "fake has a file that is also a folder: " + got[1][1]])
                continue
            assert len(world[10]) == previous_count + 1
            handles[destination] = parse(f'(cap ({cap}) (some "") (some {previous_count}))')
            outcomes.append(["returned", f"<{cap[0].upper() + cap[1:]}>" ])
            continue
        handle = handles[source]
        cap = handle[1][0]
        if name in OBSERVATIONS:
            state = world[10][int(handle[3][1])]
            lines = state[OBSERVATIONS[name]]
            program = '(Program () ((FunDecl "entry" (("x" (list (text)))) (list (text)) (var "x") "M" (function) 0)) () "entry" (noSource))'
            values = " ".join(f"(text {wire(s)})" for s in lines)
            got = oracle(root, program, world, f"(entry ((list ({values}))))")
        else:
            io, ty = OPS[name]
            args = '(var "h")'
            if name == "RANDOM_INT":
                args += f" (lit (int {low})) (lit (int {high}))"
            elif name in ("CONSOLE_PRINT", "CONSOLE_ERROR"):
                args += " " + text(value)
            elif name not in ("ARGS_LIST", "CONSOLE_READ_LINE", "CLOCK_NOW", "FILES_READ_ONLY"):
                args += " " + text(path)
                if name in ("FILES_WRITE_TEXT", "FILES_APPEND_TEXT"):
                    args += " " + text(value)
            program = f'(Program () ((FunDecl "entry" (("h" (cap ({cap})))) {ty} (prim (io ({io})) ({args})) "M" (function) 0)) () "entry" (noSource))'
            got = oracle(root, program, world, f"(entry ({wire(handle)}))")
            if got[0] == "returned" and ty.startswith("(cap"):
                handles[destination] = parse(got[1])
                kind = handles[destination][1][0]
                got[1] = f"<{kind[0].upper() + kind[1:]}>"
            if got[0] == "stopped":
                assert got[1] == ["outside", "Random.int needs low below high"], got
                got[1] = "high not greater than low"
        outcomes.append(got)
    return outcomes


def cases():
    commands = []

    def add(name, source=0, destination=0, path="", value="", low=0, high=0):
        commands.append((name, source, destination, path, value, low, high))

    add("FILES_IN_MEMORY", path="nest/a", value="original", low=1)
    add("FILES_READ_ONLY", destination=1)
    add("FILES_WITHIN", destination=2, path="nest")
    add("READFILES_WITHIN", source=1, destination=3, path="nest")
    for name in OPS:
        if name.startswith(("FILES_", "READFILES_")) and not name.endswith(("WITHIN", "READ_ONLY")):
            is_ro = name.startswith("READFILES")
            for path in ["nest/a", "nest", ".", "missing/x", "../a", "nest//a", "a/../nest/a", "x\0y"]:
                add(name, source=1 if is_ro else 0, path=path, value="updated\n🙂")
    add("FILES_MAKE_FOLDER", path="future/deep")
    add("FILES_WRITE_TEXT", path="future/deep/z", value="last")
    add("FILES_LIST", path="future")
    add("FILES_ENTRIES", path="future/deep")
    add("FILES_WRITE_TEXT", source=2, path="a", value="through child")
    add("READFILES_READ_TEXT", source=3, path="a")
    add("FILES_IN_MEMORY", destination=4, path="a/b", value="a", low=2)
    add("FILES_READ_TEXT", path="nest/a")
    add("CONSOLE_FAKE", destination=5)
    add("CONSOLE_FAKE_INPUT", destination=6, path="first\0🙂", value="second\n")
    for name in ["CONSOLE_PRINT", "CONSOLE_ERROR", "CONSOLE_PRINTED", "CONSOLE_PRINTED_ERRORS",
                 "CONSOLE_READ_LINE", "CONSOLE_READ_LINE", "CONSOLE_READ_LINE"]:
        add(name, source=6, value="line\0🙂\n")
    add("CONSOLE_PRINTED", source=5)
    add("ARGS_OF", destination=7, path="arg one", value="arg\0two")
    add("ARGS_LIST", source=7)
    add("ENV_OF", destination=8, path="KEY", value="value", low=1)
    for key in ["KEY", "key", "missing", ""]:
        add("ENV_GET", source=8, path=key)
    for instant in [-62135596800000000, -1, 0, 253402300799999999]:
        add("CLOCK_FIXED", destination=9, low=instant)
        add("CLOCK_NOW", source=9)
    for seed in [0, 1, -1, -2**63, 2**63 - 1]:
        add("RANDOM_SEEDED", destination=10, low=seed)
        for low, high in [(0, 100), (-2**63, 2**63 - 1), (-7, 2), (0, 2**62 + 1), (4, 5)] * 8:
            add("RANDOM_INT", source=10, low=low, high=high)
        add("RANDOM_INT", source=10, low=3, high=3)
    rng = random.Random(237)
    for _ in range(100):
        add(rng.choice(["FILES_WRITE_TEXT", "FILES_APPEND_TEXT", "FILES_MAKE_FOLDER",
                        "FILES_LIST", "FILES_ENTRIES", "FILES_REMOVE", "FILES_READ_TEXT"]),
            path=rng.choice(["a", "nest/a", "new/deep", ".", "./new", "new/../a"]),
            value=str(rng.randrange(-1000, 1000)))
    assert {c[0] for c in commands} == set(ROWS)
    return commands


def main():
    commands = cases()
    inputs = "".join(f"{IDS[name]} {source} {destination} {path.encode().hex() or '-'} {value.encode().hex() or '-'} {low} {high}\n"
                     for name, source, destination, path, value, low, high in commands)
    with tempfile.TemporaryDirectory(prefix="mo-host-") as directory:
        want = expected(pathlib.Path(directory), commands)
        for driver in sys.argv[1:]:
            result = subprocess.run([driver, "--sequence"], input=inputs, capture_output=True, text=True, timeout=120)
            assert result.returncode == 0, (driver, result.stderr)
            got = [[kind, bytes.fromhex(value).decode()] for kind, value in
                   (line.split(" ", 1) for line in result.stdout.splitlines())]
            assert len(got) == len(want), (driver, len(got), len(want))
            for command, actual, expected_value in zip(commands, got, want):
                assert actual == expected_value, (driver, command, actual, expected_value)
            print(f"host differential: {driver}: {len(want)} persistent fake outcomes match mocore; all {len(ROWS)} operations")


if __name__ == "__main__":
    main()
