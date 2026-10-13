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

from check_core_check import program, signatures


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
    return f'(if_ (var "b") {a} {b})'


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


def join_program(i):
    rng = random.Random(641 + i)
    ty, fallback = rng.choice([
        ('(int)', '(lit (int -29))'),
        ('(text)', '(lit (text "other\\u{0}🙂"))'),
        ('(bytes)', '(lit (bytes (0 128 255)))'),
        ('(tuple ((text) (int)))', '(tuple ((lit (text "other")) (lit (int 7))))'),
        ('(option (text))', '(con (option (text)) "Some" ((lit (text "other"))))'),
        ('(list (text))', '(prim (list (text) (lit 1)) ((lit (text "item"))))'),
        ('(fn () (text))', '(lam () (lit (text "other")))'),
        ('(cap (console))', '(var "x")'),
    ])
    body = '(if_ (var "again") (jump "loop" ((var "q") (lit (bool false)))) ' + \
           f'(if_ (var "b") (var "q") {fallback}))'
    rest = f'(if_ (var "b") (jump "loop" ((var "x") (lit (bool true)))) {fallback})'
    variant = i % 6
    if variant == 0:
        # A self-edge does not make an unused body reachable, even with reference parameters.
        body = '(jump "loop" ((var "q") (var "again")))'
        rest = f'(if_ (var "b") (var "x") {fallback})'
    elif variant == 1:
        # Nested jumps can discard the inner join's locals and reach an outer declaration.
        body = f'(join "inner" (("r" {ty})) (if_ (var "b") ' + \
               '(jump "loop" ((var "r") (lit (bool false)))) (var "r")) ' + \
               '(jump "inner" ((var "q"))))'
    elif variant == 2:
        # The second lexical label shadows the first; its rest still sees outer parameters.
        body = f'(join "loop" (("q" {ty})) (var "q") (jump "loop" ((var "q"))))'
    elif variant == 3:
        # First-match duplicate parameters survive staging and simultaneous assignment.
        body = f'(join "inner" (("q" {ty}) ("q" {ty})) (var "q") ' + \
               f'(jump "inner" ((var "q") {fallback})))'
    elif variant == 4:
        # Stop while evaluating a later argument, after an earlier reference was saved.
        rest = '(jump "loop" ((var "x") (stop "later argument stopped")))'
    else:
        body = '(if_ (var "again") (jump "loop" ((var "q") (lit (bool false)))) ' + \
               '(stop "last arm stopped"))'
    return f'''(Program () (
      (FunDecl "start" (("b" (bool)) ("x" {ty})) {ty}
        (let_ "keep" (var "x") (let_ "chosen"
          (join "loop" (("q" {ty}) ("again" (bool))) {body} {rest})
          (call "identity" ((var "chosen"))))) "Generated" (function) 0)
      (FunDecl "identity" (("x" {ty})) {ty} (var "x") "Generated" (function) 0)
    ) () "start" (noSource))'''


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


def closure_program(i):
    rng = random.Random(543 + i)
    ty, value, other = rng.choice([
        ('(int)', '(lit (int 17))', '(lit (int -29))'),
        ('(text)', '(lit (text "A\\u{0}🙂"))', '(lit (text "other"))'),
        ('(bytes)', '(lit (bytes (0 128 255)))', '(lit (bytes (37 19)))'),
        ('(tuple ((text) (int)))', '(tuple ((lit (text "left")) (lit (int 7))))',
         '(tuple ((lit (text "right")) (lit (int 23))))'),
        ('(option (text))', '(con (option (text)) "Some" ((lit (text "first"))))',
         '(con (option (text)) "None" ())'),
    ])
    fn = f'(fn ({ty}) {ty})'
    forms = [
        f'(app (lam (("z" {ty})) (var "z")) ({value}))',
        f'(app (lam (("z" {ty})) (if_ (var "b") (var "x") (var "z"))) ({value}))',
        f'(app (app (lam (("z" (int))) (lam (("y" (int))) (var "x"))) '
        '((lit (int 7)))) ((lit (int -23))))',
        f'(let_ "f" (lam () (var "x")) (let_ "x" {other} (app (var "f") ())))',
        f'(let_ "f" (lam (("x" {ty})) (var "x")) '
        '(let_ "y" (app (var "f") ((var "x"))) (app (var "f") ((var "y")))))',
        f'(app (lam (("z" {ty})) (call "identity" ((var "z")))) ((var "x")))',
        f'(let_ "f" (if_ (var "b") (lam (("z" {ty})) (stop "stop")) '
        f'(lam (("z" {ty})) (var "z"))) (app (var "f") ((var "x"))))',
        f'(case_ (con (option {fn}) "Some" ((lam (("z" {ty})) (var "z")))) '
        f'(("None" () {value}) ("Some" ("f") (app (var "f") ((var "x"))))))',
        f'(app (lam (("z" {ty}) ("z" {ty})) (var "z")) ({value} {other}))',
        f'(let_ "x" {value} (app (lam () (let_ "x" {other} '
        '(let_ "f" (lam () (var "x")) (app (var "f") ())))) ()))',
    ]
    body = forms[i % len(forms)]
    if i % 11 == 0:
        # A closure-valued global, and a closure application during initialization.
        return f'''(Program () (
          (FunDecl "start" () {ty} (global "answer") "Generated" (function) 2)
        ) (
          (ConstDecl "f" {fn} (lam (("x" {ty})) (var "x")))
          (ConstDecl "answer" {ty} (app (global "f") ({value})))
        ) "start" (noSource))'''
    return f'''(Program () (
      (FunDecl "start" (("b" (bool)) ("x" {ty})) {ty} {body} "Generated" (function) 0)
      (FunDecl "identity" (("x" {ty})) {ty} (var "x") "Generated" (function) 0)
    ) () "start" (noSource))'''


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
        for i in range(384):
            path = pathlib.Path(directory) / f"closures-{i}.core"
            path.write_text(f"(core {version} {closure_program(i)})\n")
            generated.append(path)
        for i in range(192):
            path = pathlib.Path(directory) / f"joins-{i}.core"
            path.write_text(f"(core {version} {join_program(i)})\n")
            generated.append(path)
        excluded = []
        for i, (prim, types, ret, spec) in enumerate(signatures()):
            # These two operations need instruction expansions, not new primitive rows.
            if prim in ['(range (toText))', '(list (byte) (uncons))']:
                continue
            params = [(f"arg{j}", t) for j, t in enumerate(types)]
            args = ' '.join(f'(var "arg{j}")' for j in range(len(types)))
            body = f'(prim {prim} ({args}))'
            if spec:
                body = f'(let_ "discard" {body} (lit (int 0)))'
                params.append(("result", "(int)"))
            payload = program(body, "(int)" if spec else ret, params,
                              "promise" if spec else "function")
            path = pathlib.Path(directory) / f"operation-{i}.core"
            path.write_text(f"(core {version} {payload})\n")
            if prim.startswith(('(eq (fn ', '(eq (cap ', '(eq (data ')):
                # Lean types these, but source lowering cannot produce runtime equality on them.
                excluded.append(path)
            else:
                generated.append(path)
        for driver in sys.argv[1:]:
            print(f"codegen: {driver}: {len(paths)} fixture/corpus inputs")
            run(driver, paths)
            supported = [pathlib.Path("tests/core") / f"{name}.core"
                         for name in ["Account", "Greet", "Metadata", "RateLimit"]]
            assert run(driver, supported) == (len(supported), 0, 0, 0, 0, 0), "a supported fixture was not compiled"
            assert run(driver, generated) == (len(generated), 0, 0, 0, 0, 0), "a supported generated program was not compiled"
            assert run(driver, excluded) == (0, 0, len(excluded), 0, 0, 0), "a lowering-impossible operation needs E1201"
    print(f"codegen: all generated programs compiled; corpus coverage reported separately ({time.monotonic() - started:.2f}s)")


if __name__ == "__main__":
    main()
