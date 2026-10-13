#!/usr/bin/env python3
"""Compile checked Core fixtures/corpora and generated programs, twice.

The staged compiler skips lowering-impossible representations (E1201) and counts unfinished
forms separately. Every generated program must compile through the verifying writer, not
merely get a stable refusal or a resource limit. Core's checker differential still tests
the broader Lean language, including the representations excluded from compilation.
No Core evaluation: outcome comparison belongs to the VM stream.
"""

import pathlib
import random
import re
import subprocess
import sys
import tempfile
import time


def expression(rng, depth):
    if depth == 0:
        return rng.choice(['(var "x")', '(lit (int -17))', '(lit (int 29))'])
    choice = rng.randrange(7)
    a = expression(rng, depth - 1)
    b = expression(rng, depth - 1)
    if choice == 0:
        return f'(let_ "x" {a} {b})'
    if choice == 1:
        return f'(if_ (var "b") {a} {b})'
    if choice == 2:
        return f'(call "identity" ({a}))'
    if choice == 3:
        return f'(if_ (var "b") (stop "generated stop") {a})'
    if choice == 4:
        return f'(case_ (con (option (int)) "Some" ({a})) (("None" () {b}) ("Some" ("x") {b})))'
    if choice == 5:
        return f'(case_ (con (result (int) (bool)) "Error" ((var "b"))) (("Ok" ("x") {a}) ("Error" ("b") {b})))'
    return f'(case_ (con (entryKind) "Link" ()) (("File" () {a}) ("Folder" () {b}) ("Link" () {a}) ("Other" () {b})))'


def reference_value(rng, depth):
    if depth == 0:
        return rng.choice(['(var "x")', '(lit (text "a\\u{0}🙂"))', '(lit (text "second"))'])
    a = reference_value(rng, depth - 1)
    b = reference_value(rng, depth - 1)
    choice = rng.randrange(4)
    if choice == 0:
        return f'(let_ "x" {a} {b})'
    if choice == 1:
        return f'(call "identity" ({a}))'
    if choice == 2:
        return f'(proj (tuple ({a} {b})) {rng.randrange(2)})'
    # Reconverging arms with no local owners are supported, including literal/variable results.
    return '(if_ (var "b") (var "x") (lit (text "fallback")))'


def reference_tail(rng, depth):
    if depth == 0:
        return reference_value(rng, 2)
    a = reference_tail(rng, depth - 1)
    b = reference_tail(rng, depth - 1)
    value = reference_value(rng, 2)
    choice = rng.randrange(4)
    if choice == 0:
        return f'(let_ "x" {value} {a})'
    if choice == 1:
        return f'(if_ (var "b") {a} {b})'
    if choice == 2:
        return f'(case_ (con (option (text)) "Some" ({value})) (("None" () {a}) ("Some" ("x") {b})))'
    return f'(case_ (con (data "Pair") "Pair" ({value} {value})) (("Pair" ("x" "y") {a})))'


def global_program(i):
    rng = random.Random(104 + i)
    ty, first, fallback = rng.choice([
        ('(int)', '(lit (int 17))', '(lit (int -31))'),
        ('(text)', '(lit (text "A\\u{0}🙂"))', '(lit (text "other"))'),
        ('(bytes)', '(lit (bytes (0 128 255)))', '(lit (bytes (17 29)))'),
        ('(range)', '(prim (range (make)) ((lit (int -7)) (lit (int 23))))',
         '(prim (range (make)) ((lit (int 19)) (lit (int 2))))'),
        ('(tuple ((text) (int)))', '(tuple ((lit (text "first")) (lit (int 37))))',
         '(tuple ((lit (text "fallback")) (lit (int -9))))'),
        ('(option (text))', '(con (option (text)) "Some" ((lit (text "first"))))',
         '(con (option (text)) "None" ())'),
    ])
    second = rng.choice([
        '(global "first")',
        '(call "relay" ())',
        f'(if_ (lit (bool false)) (global "first") {fallback})',
        f'(case_ (con (option {ty}) "Some" ((global "first"))) '
        f'(("None" () {fallback}) ("Some" ("value") (var "value"))))',
        f'(let_ "value" (global "first") '
        f'(if_ (lit (bool true)) (var "value") {fallback}))',
    ])
    # A stopping body must still produce a valid init wrapper, not an unreachable store.
    if i % 17 == 0:
        first = '(stop "initialization stopped")'
    return f'''(Program () (
      (FunDecl "start" () (tuple ({ty} {ty})) (global "third") "Generated" (function) 3)
      (FunDecl "relay" () {ty} (global "first") "Generated" (function) 1)
    ) (
      (ConstDecl "first" {ty} {first})
      (ConstDecl "second" {ty} {second})
      (ConstDecl "third" (tuple ({ty} {ty})) (tuple ((global "first") (global "second"))))
    ) "start" (noSource))'''


def run(driver, paths):
    result = subprocess.run([driver, *map(str, paths)], text=True, capture_output=True, timeout=60)
    print(result.stdout, end="")
    if result.returncode:
        raise AssertionError(f"{driver}: {result.stderr}")
    counts = re.search(r"(\d+) compiled deterministically; (\d+) explicitly unsupported; (\d+) lowering-impossible inputs skipped; (\d+) entry-name collisions; (\d+) resource limits; (\d+) not checked programs", result.stdout)
    assert counts, result.stdout
    return tuple(map(int, counts.groups()))


def main():
    started = time.monotonic()
    assert sys.argv[1:], "a built test_codegen driver is required"
    version = re.search(r'#define MO_CORE_TEXT_VERSION "([^"]+)"', pathlib.Path("src/core/core.h").read_text())[1]
    paths = []
    for root in [pathlib.Path("tests/core"), *[pathlib.Path(base) / name
                 for base in ["fuzz/corpus", "build/fuzz/corpus"]
                 for name in ["fuzz_core_text", "fuzz_core_check", "fuzz_codegen"]]]:
        if root.exists():
            paths.extend(sorted(p for p in root.rglob("*") if p.is_file()))
    with tempfile.TemporaryDirectory(prefix="mo-codegen-") as directory:
        generated = []
        for i in range(256):
            body = expression(random.Random(207 + i), i % 4)
            entry = "start" if i % 2 else "main"
            payload = f'''(Program () (
              (FunDecl "{entry}" (("b" (bool)) ("x" (int))) (int) {body} "Generated" (function) 0)
              (FunDecl "identity" (("x" (int))) (int) (var "x") "Generated" (function) 0)
            ) () "{entry}" (noSource))'''
            path = pathlib.Path(directory) / f"{i}.core"
            path.write_text(f"(core {version} {payload})\n")
            generated.append(path)
        for i in range(256):
            body = reference_tail(random.Random(540 + i), i % 4)
            entry = "reference_entry" if i % 2 else "main"
            payload = f'''(Program ((DataDecl "Pair" ((Ctor "Pair" (("a" (text)) ("b" (text))))))) (
              (FunDecl "{entry}" (("b" (bool)) ("x" (text))) (text) {body} "Generated" (function) 0)
              (FunDecl "identity" (("x" (text))) (text) (var "x") "Generated" (function) 0)
            ) () "{entry}" (noSource))'''
            path = pathlib.Path(directory) / f"reference-{i}.core"
            path.write_text(f"(core {version} {payload})\n")
            generated.append(path)
        for i in range(192):
            path = pathlib.Path(directory) / f"globals-{i}.core"
            path.write_text(f"(core {version} {global_program(i)})\n")
            generated.append(path)
        for driver in sys.argv[1:]:
            print(f"codegen: {driver}: {len(paths)} fixture/corpus inputs")
            run(driver, paths)
            assert run(driver, generated) == (len(generated), 0, 0, 0, 0, 0), "a supported generated program was not compiled"
    print(f"codegen: all generated programs compiled; corpus coverage reported separately ({time.monotonic() - started:.2f}s)")


if __name__ == "__main__":
    main()
