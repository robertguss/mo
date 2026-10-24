# Section 14 inventory: which core library functions run

MO-135 probed every function Language Reference v0.1 section 14 lists, except `Time`, `Duration`,
`Clock` and Int's `seconds` through `days`, which MO-134 covers (#196). Each function got its own minimal
program, run through `mo test` (which checks it first) on main `fc53746` and again with this
change. A row says "runs" only when each form below checked and ran.

## Forms probed

For a function `name` on type `T`, with receiver `r` and arguments `a`:

- dot: `r.name(a)`, or `r.name` with no arguments (reference 5.4);
- bound: `x = r` and then `x.name(a)`;
- qualified: `T.name(r, a)`, and `T.name(a)` for the functions section 14 writes that way
  (`Text.from_chars`, `Console.fake`, `Files.in_memory`, `Json.parse`, ...);
- value: `f = T.name` and then `f(r, a)`.

Each result is printed through interpolation, so its `Text` form runs too. Fakes are made inside a
`test`, as reference 12.4 requires. Section 14's opening paragraph gives every type with `Text`
a `to_text` and every type with `Order` a `compare`; those were probed for each built-in type,
including `Bool`, `Unit`, tuples, `Ordering`, `Entry`, `EntryKind` and `FileError`.

## Results

| Type | Functions | Result | Golden programs |
| --- | --- | --- | --- |
| Int | every function except the Duration constructors | runs in every form | `num-int-functions`, `num-int-checked`, `ref-library-natives`, `ref-abilities` |
| BigInt | operators, its seven functions, `to_text`, `compare` | runs; qualified `compare`: MO-144 | `num-bigint`, `ref-bigint` |
| Byte | `to_int`, `to_text`, `compare` | runs; qualified `compare`: MO-144 | `ref-library-natives`, `ref-abilities` |
| Decimal | every function | runs; qualified `compare`: MO-144 | `num-decimal-rounding`, `num-decimal-int` |
| Float | every function | runs; qualified `compare`: MO-144 | `num-float-convert`, `ref-library-natives` |
| Text | every function and `Text.from_chars` | runs; qualified `to_text` and `compare`: MO-144 | `txt-*`, `ref-library-natives` |
| Char | every function | runs; qualified `compare`: MO-144 | `txt-case-chars`, `txt-chars-roundtrip` |
| Bytes | `size`, `get`, `decode_utf8`, `compare` | runs; qualified `compare`: MO-144 | `ref-library-natives`, `ref-abilities` |
| List | every function, `join` and `to_map` | runs; qualified `to_text` and `compare`: MO-144 | `col-list-*`, `ref-library-natives` |
| Map | every function and `to_text` | runs; qualified `to_text`: MO-144 | `col-map-*`, `ref-abilities` |
| Set | every function, `Set.empty` and `to_text` | runs; qualified `to_text`: MO-144 | `col-set-basics` |
| Range | `for`, `to_list`, `contains?`, `to_text` | runs; qualified `to_text`: MO-144 | `col-range-loops` |
| Option | `or`, `or_fail`, `map`, `to_text`, `compare` | runs; qualified `to_text` and `compare`: MO-144 | `fail-option-result-map`, `ref-evaluation` |
| Result | `map`, `map_error`, `to_text` | runs; qualified `to_text`: MO-144 | `fail-option-result-map` |
| Ordering, Unit, EntryKind | `to_text`, `compare` | runs, including direct receivers such as `Less.compare(Greater)` since MO-130 (#202); qualified: MO-144 | `lang-dot-calls-cases`, `ref-case-receivers`, `ref-abilities` |
| Bool, tuples | `to_text`, `compare` | runs; qualified `Bool` forms: MO-144 | `ref-abilities` |
| Args, Env, Random | `list`, `get`, `int` and the fakes | runs | `cap-args-list`, `ref-capability-fakes` |
| Console | `print`, `error`, `read_line`, `printed`, `printed_errors`, the fakes | runs | `cap-console-*` |
| Files, ReadFiles | every operation and `Files.in_memory` | runs | `cap-files-*` |
| FileError, Entry | constructors, `to_text`, `compare` | runs; qualified `compare` and `Entry` `to_text`: MO-144 | `ref-abilities`, `fail-result-declared` |
| Json | `parse`, `int`, `decimal`, `float`, `to_text`, `pretty`, JsonNumber's conversions, JsonError | runs | `lib-json-query`, `lib-json-errors` |
| Unicode16 | every function | runs | `lib-unicode16` |

## What did not run, and what happened

| Form | On main `fc53746` | Now |
| --- | --- | --- |
| A case without fields written as the receiver: `Less.to_text`, `Less.compare(Greater)`, `Unit.to_text`, `Folder.to_text`, a declared `Stop.to_text` | E1103: the checker read `Less` as a module prefix | runs since MO-130 (#202); `lang-dot-calls-cases`, and `ref-case-receivers` as extra coverage |
| Qualified or value `to_text` and `compare` other than `Int.compare` and the scalar `to_text` primitives, such as `Text.compare(a, b)` or `List.to_text(xs)` | E1103 | needs a design decision: MO-144 |

Two refusals the probe met are right in outcome but give E1103 rather than a specific error. Making
a fake in `main` or another function, which reference 12.4 forbids, belongs to MO-143. A qualified
name no module owns, such as `Shapes.bogus(p)`, belongs to MO-144. Neither is a library function
that fails to run.
