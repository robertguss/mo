# The executable grammar

Mo's grammar, as a program: `mo-grammar` reads Mo source and reports whether it
parses. It checks:

- every code example in the Language Reference (`docs/reference/README.md`);
- the standard library (`lib/`), sample programs (`examples/`), tools (`tools/`), and services (`services/`);
- the grammar's own positive and negative test suites (`grammar/tests/`) and benchmark v0 (`bench/v0/`).

It is an instrument, not the compiler (`crates/mo-syntax/`). It checks syntax only:
types, names in scope, abilities, capabilities, and exhaustive `case` checks belong
to `crates/mo-check/`.

## The parts

- `mo.y` is the grammar, and the authority on what Mo's syntax accepts. Bison
  builds an LR(1) parser from it (IELR, with no precedence declarations) and
  refuses any grammar with a conflict, so a grammar that builds is
  unambiguous.
- `lexer.c` reads characters into tokens. It decides where lines end, which
  is where statements end (reference 2.4), and reads names, numbers, text, and
  interpolation. It also rejects invisible characters and invalid UTF-8.
- `ast.c` checks what a context-free grammar cannot express: what may be a
  pattern, what may stand on the left of `=`, and how records and cases are
  built.
- `main.c` is the command.

## Using it

```sh
make -C grammar grammar                        # build debug + sanitizer mo-grammar and run every check
build/grammar/mo-grammar FILE.mo               # check a module
build/grammar/mo-grammar --fragment FILE       # declarations and statements in any order
build/grammar/mo-grammar NOTES.md              # every ```mo block, as a fragment
```

Errors read `file:line:column: message`. A word another language uses where
Mo has its own, such as `def` or `elif`, gets advice in the message.

## Tests

- `tests/good/` holds fragments that must parse.
- `tests/bad/` (fragments) and `tests/bad-files/` (modules) hold code that must
  fail. Each file's first line, `# error: TEXT`, names the error, and `TEXT`
  may start with `line:column:`.

A change to the grammar comes with tests, matching `crates/mo-syntax/` parser updates,
and the Language Reference updated to match.
