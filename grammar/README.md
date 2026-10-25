# The executable grammar

Mo's grammar, as a program: `mo-grammar` reads Mo source and says whether it
parses. It checks three things:

- every code example in the Language Reference (`docs/reference/README.md`);
- the sample programs in `examples/`;
- in the learnability test, whether an agent's answer parses.

It is an instrument, not the compiler. It checks syntax only: types, names in
scope and whether a `case` covers every value are the compiler's.

## The parts

- `mo.y` is the grammar, and the authority on what Mo's syntax accepts. Bison
  builds an LR(1) parser from it (IELR, with no precedence declarations) and
  refuses any grammar with a conflict, so a grammar that builds is
  unambiguous.
- `lexer.c` reads characters into tokens. It decides where lines end, which
  is where statements end (reference 2.4), and reads names, numbers, text and
  interpolation. It also rejects invisible characters and invalid UTF-8.
- `ast.c` checks what a context-free grammar can't express: what may be a
  pattern, what may stand on the left of `=`, and how records and cases are
  built.
- `main.c` is the command.

## Using it

```sh
make grammar                                  # build it and run every check
build/clang/debug/mo-grammar FILE.mo          # a module
build/clang/debug/mo-grammar --fragment FILE  # declarations and statements in any order
build/clang/debug/mo-grammar NOTES.md         # every ```mo block, as a fragment
```

Errors read `file:line:column: message`. A word another language uses where
Mo has its own, such as `def` or `elif`, gets advice in the message.

## Tests

- `tests/good/` holds fragments that must parse.
- `tests/bad/` (fragments) and `tests/bad-files/` (modules) hold code that must
  fail. Each file's first line, `# error: TEXT`, names the error, and `TEXT`
  may start with `line:column:`.
- `fuzz/fuzz_grammar.c` fuzzes the lexer, the parser and the checks.

A change to the grammar comes with tests, and with the reference changed to
match.
