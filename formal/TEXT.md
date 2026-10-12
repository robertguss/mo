# Core text, version 0.1

The Lean implementation in `MoCore/Text/` is the reference codec. Every file
is `(core 0.1 PAYLOAD)` followed by LF. Constructors are case-sensitive and
named after Lean. A nullary constructor is still a list: `(unit)`.

This is an interchange format, not Mo source or the informal notation in
TRANSLATION.md. There are two result representations: lossless constructor
transport (`Outcome`, including closures), and observable reports (`Report`,
the protocol `mocore` writes). The latter uses reference 9.2's structural
value spelling inside a string. C differential tests compare **reports**.

## Lexing and canonical layout

Canonical S-expressions have one ASCII space between elements, no internal
line breaks, and no space just inside parentheses. Strings use `\"`, `\\`,
`\n`, `\r`, `\t`; other controls U+0000–001F and U+007F use `\u{hex}` with
lowercase shortest hex. Other Unicode scalars print literally, without
normalization. Input accepts 1–6 hex digits (either case) in `\u{...}` and
rejects surrogates and values above U+10FFFF. Raw controls are rejected.

Whitespace is space, tab, LF or CR. `;` comments run through LF or EOF and
act as whitespace outside strings. Atoms end at whitespace, parentheses,
double quote or semicolon. Decimal whole numbers have arbitrary precision,
optional minus, and at least one ASCII digit; no plus or separators. Input
allows leading zeros and negative zero; printing removes them. Naturals must
be nonnegative. Float bits are `0x` and exactly 16 hex digits; input allows
either case, printing uses lowercase and preserves every bit, including NaN
payloads and negative zero. Names are always strings, never symbols.

Parsing is not type checking: an out-of-range integer/byte literal can be
parsed, then refused by the checker.

```ebnf
file = "(", "core", "0.1", payload, ")" ;
sexp = atom | string | "(", { sexp }, ")" ;
integer = [ "-" ], digit, { digit } ;
bits = "0x", hex-digit (* exactly sixteen *) ;
type = "(int)" | "(bigInt)" | "(byte)" | "(decimal)" | "(float)"
     | "(bool)" | "(char)" | "(text)" | "(bytes)" | "(unit)"
     | "(range)" | "(time)" | "(duration)" | "(ordering)" | "(fileError)"
     | "(tuple (", { type }, "))" | "(list ", type, ")"
     | "(map ", type, type, ")" | "(set ", type, ")"
     | "(data ", string, ")" | "(option ", type, ")"
     | "(result ", type, type, ")" | "(fn (", { type }, ") ", type, ")"
     | "(cap ", capability, ")" ;
capability = "(console)" | "(files)" | "(readFiles)" | "(clock)"
           | "(random)" | "(args)" | "(env)" ;
literal = "(", numeric-tag, integer, ")" | "(float ", bits, ")"
        | "(bool ", ("true" | "false"), ")" | "(char ", string, ")"
        | "(text ", string, ")" | "(bytes (", { integer }, "))" | "(unit)" ;
numeric-tag = "int" | "bigInt" | "byte" | "decimal" | "time" | "duration" ;
```

Decimal integers count 10⁻¹⁸ units; Time and Duration integers count
microseconds. Char strings contain exactly one Unicode scalar.

```text
(core 0.1 (map (text) (list (byte))))
(core 0.1 (float 0x8000000000000000))
(core 0.1 (text "hello\nβ"))
```

Files are UTF-8 without a BOM; invalid UTF-8 is a parse error. An atom is a
nonempty run of scalars other than the delimiters defined above. Symbols
are atoms used as constructor names, `core`, `0.1`, `true` and `false`;
only the exact symbols listed in this document are accepted in those
positions. There is no symbol escaping. Empty lists are `()`. Parsing
accepts any layout using those delimiters, including adjacent lists,
leading/trailing whitespace and comments; there must be exactly one outer
S-expression and no other non-comment input. All fields and list wrappers
are mandatory; unknown constructors and wrong arities are errors.

Canonical printing retains **every list's order**, including declarations,
parameters, fields, arms, environments, maps, sets and files. It never sorts,
renames variables, deduplicates, reorders dependencies or inserts defaults.
Equal Lean programs print byte-identical files. This is equality of syntax,
not alpha-equivalence or Mo's `==`: equal maps/sets with different insertion
orders intentionally print differently. Canonical input means output of the
printer; reparsing and printing it reproduces every byte, including final LF.

The EBNF above describes tokens, not required spaces. Below, `T` means a
type, `E` an expression, `L` a literal, `P` a primitive, `V` a lossless value,
`S` a string, `N` a natural, `I` an integer and `B` a Boolean atom. `(X*)`
means a parenthesized list of zero or more Xs; it is not a literal `*`.
Pairs and triples have their own parentheses. Structure tags use the Lean
type name (`Program`, `FunDecl`, etc.), rather than the ambiguous `mk`
constructor; inductive tags use the constructor's unqualified Lean name.
Lean's list `nil`/`cons` are represented by proper lists, not explicit tags.
`digit` is ASCII `0`–`9`, `hex-digit` is `0`–`9`, `a`–`f` or `A`–`F`.
`string` is double quote, zero or more non-control scalars other than double
quote/backslash or the escapes defined above, then double quote. `payload`
is any schema in this document; its category is supplied by the caller
(`programIn`, `valueIn`, etc.), not inferred from an untyped union. For
example `(unit)` is both a type, literal and value, according to context.

## Programs and every expression

The following schema is exhaustive, in **Lean field order**. No metadata is
implicit. A module is metadata; a function name is the resolved lookup key.

```text
Program   := (Program (DataDecl*) (FunDecl*) (ConstDecl*) S)
DataDecl  := (DataDecl S (Ctor*))
Ctor      := (Ctor S ((S T)*))
ConstDecl := (ConstDecl S T E)
FunDecl   := (FunDecl S ((S T)*) T E S Kind N)
Kind      := (function) | (test) | (example) | (promise) | (anchor)
```

`Program`'s last string is the entry. `FunDecl`'s fields are name,
parameters, result type, body, module, kind and `constCount` (the required
constant prefix). Constants are already in dependency order. The checker,
not the parser, validates prefixes, entry, kinds and bodies.

```text
E := (lit L)
   | (var S)
   | (global S)
   | (let_ S E E)
   | (lam ((S T)*) E)
   | (app E (E*))
   | (call S (E*))
   | (tuple (E*))
   | (proj E N)
   | (con T S (E*))
   | (case_ E ((S (S*) E)*))
   | (if_ E E E)
   | (prim P (E*))
   | (stop S)
   | (fail E)
   | (join S ((S T)*) E E)
   | (jump S (E*))
```

`let_` holds name, bound expression, body; `if_` holds condition, true
branch, false branch; `join` holds name, parameters, body, rest. Projection
indices are zero-based. `case_` arms are constructor name, bound field
names, body, in constructor declaration order. Empty parameter/argument/
field/arm lists are still present. No source locations are transported.

## Every primitive

```text
P := (int IntOp) | (big BigOp) | (byte ByteOp) | (dec DecOp)
   | (float FloatOp) | (not) | (cmp Base CmpOp) | (char CharOp)
   | (text TextOp) | (bytes BytesOp) | (range RangeOp) | (time TimeOp)
   | (list T ListOp) | (map T T MapOp) | (set T SetOp) | (eq T) | (io IoOp)
```

Each name in the following table means a **nullary S-expression**, e.g.
`(checkedAdd)`, not an atom. The lists are exhaustive; casing matters.

| Family | Nullary constructors |
| --- | --- |
| `IntOp` | `add sub mul div rem neg abs min max pow floorDiv floorMod checkedAdd checkedSub checkedMul wrappingAdd wrappingSub wrappingMul saturatingAdd saturatingSub saturatingMul toText toDecimal toBig toFloat toChar toByte seconds milliseconds minutes hours days` |
| `BigOp` | `add sub mul div rem neg abs toInt toText` |
| `ByteOp` | `toInt toText` |
| `DecOp` | `add sub mul div rem neg abs min max floor ceil round toInt toText toFixed toFloat` |
| `FloatOp` | `add sub mul div neg abs sqrt floor ceil round isNan isInfinite toText` |
| `Base` | `int bigInt byte decimal float bool char text bytes time duration` |
| `CmpOp` | `lt le gt ge` |
| `CharOp` | `code isDigit isLetter isWhiteSpace lower upper toText` |
| `TextOp` | `concat byteCount charCount isEmpty chars fromChars lines words split trim trimStart trimEnd startsWith endsWith contains dropPrefix dropSuffix replace repeat padLeft padRight lower upper toInt toDecimal join bytes` |
| `BytesOp` | `size get decodeUtf8` |
| `RangeOp` | `make start end toList contains toText` |
| `TimeOp` | `sub add subDur checkedAdd toText parse durAdd durSub durMul durNeg inMilliseconds inSeconds durToText` |
| `ListOp` | `size isEmpty get first last append prepend concat reverse take drop set removeAt uncons enumerate contains unique` |
| `MapOp` | `empty size isEmpty get has set remove keys values entries ofList` |
| `SetOp` | `empty size isEmpty has add remove union intersection difference toList ofList` |
| `IoOp` | `argsList envGet print printError readLine printed printedErrors writeText appendText remove readOnly now randomInt` |

The non-nullary operation constructors are exactly:

```text
ListOp additionally := (lit N) | (zip T)
MapOp  additionally := (lit N)
IoOp   additionally := (readText B) | (readBytes B) | (list B) | (exists B) | (within B)
```

`ListOp.lit` counts values; `MapOp.lit` counts entries (twice that many
expression arguments, alternating key/value). `zip` carries the second
list's element type. I/O's Boolean selects `ReadFiles` when true, `Files`
when false. The guillemets around Lean's reserved identifiers `repeat`,
`end` and `exists` are **not** part of their wire spelling. For example:

```text
(prim (map (text) (int) (lit 2)) ((lit (text "a")) (lit (int 1)) (lit (text "b")) (lit (int 2))))
(prim (io (readBytes true)) ((var "files") (lit (text "a.txt"))))
(prim (list (byte) (zip (text))) ((var "bytes") (var "names")))
```

## Lossless values, worlds and internal outcomes

Lossless values have **all** `Value` constructors. The twelve literal
constructors have exactly the same syntax as literals. Additional forms:

```text
V additionally := (range I I)
   | (tuple (V*)) | (list (V*)) | (map ((V V)*)) | (set (V*))
   | (con S (V*))
   | (closure ((S V)*) ((S T)*) E ((S V)*))
   | (cap capability root)
root := (none) | (some S)
World := (World (S*) ((S S)*) (S*) (S*) (S*) ((S S)*) I N)
Bug := (overflow S) | (divByZero) | (stop S) | (outside S) | (repeatedKey)
Outcome := (ok V World) | (stopped Bug World) | (failed S World)
         | (jumped S (V*) World) | (wrong S) | (outOfFuel)
```

Closure fields are captured local environment (innermost first), parameters,
body, captured constant environment (declaration order). No inferred types
or spec mode are added. Capability roots are `some ""` for the original
folder, `some "notes"` for a narrowed folder, or `none` for no access; other
capabilities ignore root. A range carries both ends, even when empty.
Constructor values carry no type: their expected type supplies it.

World fields, in order: **arguments, environment, remaining standard input
lines, standard output lines, standard error lines, files, clock
microseconds since Unix epoch, random seed**. Environment and files are
ordered string pairs; duplicate keys are preserved (the model's lookup
finds the first). Files hold UTF-8 texts, not arbitrary bytes. Input lines
are already split: the codec does not remove CR/LF or replace invalid
UTF-8 inside a line. Print calls contribute one list element, even when
that element contains newlines. Initial output/error can be nonempty and
are preserved. Worlds deliberately accept any clock integer and natural
seed; they are host state, not checked literals. The existing primitives
apply their own rules to that state.

Lossless codecs can inspect malformed values and internal outcomes. They
do not certify value typing. `jumped` and `wrong` are provided for debugging;
a correctly selected, checked run with valid host arguments cannot finish
with either. Fuel exhaustion has **no world** in the current model, so
the codec must not invent partial effects for it.

## Structural values and observable reports

`structural P T v` prints a value using its expected type and the program's
data declarations. It never calls a custom `to_text`: a result comparison
must see the fields. Its exact forms follow reference 9.2:

| Value | Structural spelling |
| --- | --- |
| Int, BigInt, Byte | shortest decimal integer, no plus or leading zeros |
| Decimal | shortest exact decimal, no exponent, trailing fractional zeros removed; no point if whole |
| Float | `0x` followed by 16 lowercase hex digits of the bits, including NaNs and signed zero |
| Bool | `true` or `false` |
| Text, Char at top level | the text or character itself, without quotes |
| Text inside any other value | double-quoted with the string escapes above |
| Char inside any other value | single-quoted; string escapes above, plus `\'` for apostrophe (double quote is `\"`) |
| Bytes (no Text ability; oracle extension) | `bytes[0, 255]`; decimal bytes, empty `bytes[]` |
| Unit | `Unit` |
| Range | `-2..3`, including reversed/empty ends |
| Time | UTC RFC 3339, four-digit year, `T` and `Z`, shortest fractional microseconds only if nonzero |
| Duration | shortest exact seconds followed by `s`, e.g. `0s`, `-0.000001s`, `1.5s` |
| Tuple | `(1, true)`, empty `()`, singleton `(1)` |
| List | `[1, 2]`, empty `[]` |
| Map | `{"a": 1, "b": 2}`, empty `{}`, insertion order |
| Set | `[2, 1].to_set`, empty `[].to_set`, insertion order |
| Declared data / FileError | `Point(x: 1, y: 2)` / `NotFound(path: "a")`: constructor and field names in declaration order |
| Option / Result / Ordering | positional fields: `None`, `Some(3)`, `Ok(Unit)`, `Error(e)`, `Less`, `Equal`, `Greater` |
| Closure (no Text ability) | `<function>`; apply it to compare its behavior |
| Capability (no Text ability) | its lossless S-expression, e.g. `(cap (readFiles) (some "notes"))` |

Nullary data cases print the constructor alone. Constructors/field names
are copied verbatim from their declarations, including instantiated names;
the result is an observation, not Mo source intended for recompilation.
The declared field name `"0"` denotes an unnamed field (TRANSLATION 4.2)
and is positional: `Circle(1.5)`, not `Circle(0: 1.5)`.
Inside composites every element uses its **nested** form recursively.
Separators are `, ` and `: `, with no spaces just inside delimiters.

**The report stores this structural spelling as a string.** For example a
returned list of texts contains two levels of quoting:

```text
(core 0.1 (returned "[\"a\", \"b\\n\"]" (effects () () ())))
```

This avoids confusing a top-level Text with a constructor/number, and
allows every Core string name without changing the S-expression lexer.
`reportIn` parses this string as an observation, not back into a `Value`.
Reference spelling alone loses type distinctions (`Int`/`BigInt`/`Byte`),
and `<function>` loses captures; use `valueIn`/`valueOut` for reconstruction.
The full report grammar (not the internal Outcome grammar) is:

```text
effects := (effects (S*) (S*) ((S S)*))
Report := (returned S effects) | (stopped Bug effects) | (failed S effects)
        | (outOfFuel) | (wrong S) | (parseError S) | (illTyped S)
        | (invalidRun S) | (ioError S)
```

Effects are standard output, standard error and final files, each in model
order; no remaining stdin, environment, clock or seed is compared. Output
includes unchanged files. A stop or failure retains effects up to that point.
Fuel exhaustion and diagnostic reports cannot provide effects. Diagnostic
details explain errors but are not stable cross-toolchain comparison keys;
compare the diagnostic tag, not Lean's checker wording or OS error wording.

## One file describes a run; mocore runs it

```text
Run := (Run Program World Selection N)
Selection := (entry (V*)) | (test S)
```

`N` is evaluator fuel: a depth bound reused for each initializer and the
entry/test call, not a shared instruction count. The program always passes
`checkProgram` before any evaluation. Entry arguments are explicit values
in parameter order; `test` selects an existing test **or example**, not a
promise or anchor. Each invocation initializes constants once in a fresh
run, retaining their effects. Entry wrapper generation and interpreting a
Mo return value as an OS exit status remain lowering/runtime work.

Host argument validation checks scalar ranges, constructor and tuple arity,
recursive types and map/set distinctness. Closures, also nested ones, are
refused as host arguments: their environments have no type annotations from
which the runner could certify lexical spec mode. They can still be
transported and programs can create and apply them. Invalid arguments or
selection produce `invalidRun`, not an evaluator `wrong`.

```sh
cd formal
lake build                  # includes the native executable
lake exe mocore run.core
lake exe mocore program.core world.core
lake exe mocore program.core world.core --test 'greeting.1' --fuel 10000
```

For repeated differential tests, invoke `.lake/build/bin/mocore` directly
to avoid Lake startup. In the two-file form the payloads must be `Program`
and `World`. Default fuel is 1000000. Without `--test`, each entry parameter
must be a capability; the host supplies `cap c (some "")` in parameter
order. Non-capability entry parameters require a single Run file. Repeated
CLI options take the last value. Syntax/file decoding errors are
`parseError`; bad options are `invalidRun`; unavailable files are `ioError`.

The command writes exactly one canonical report to actual standard output.
Modeled program output is in that report, never mixed into the host stream.
The process exit status identifies the report, **not** Mo's main result:

| Report | Exit |
| --- | ---: |
| returned | 0 |
| stopped | 1 |
| failed | 2 |
| outOfFuel | 3 |
| parseError | 4 |
| illTyped | 5 |
| invalidRun | 6 |
| wrong (internal invariant failure) | 7 |
| ioError | 8 |

## Worked runs

A complete one-file program returning a Float's negative zero, with a
preexisting file and one input line (layout expanded for reading):

```text
(core 0.1
  (Run
    (Program ()
      ((FunDecl "main" () (float) (lit (float 0x8000000000000000))
        "Main" (function) 0))
      () "main")
    (World ("argument") (("MODE" "test")) ("input") () () (("a" "hi\n")) 0 42)
    (entry ()) 30))
```

The exact canonical report (exit 0):

```text
(core 0.1 (returned "0x8000000000000000" (effects () () (("a" "hi\n")))))
```

A named test demonstrating failure without a bug:

```text
(core 0.1
  (Run
    (Program ()
      ((FunDecl "main" () (unit) (lit (unit)) "Main" (function) 0)
       (FunDecl "expected seven" () (unit) (fail (lit (text "got six"))) "Spec" (test) 0))
      () "main")
    (World () () () () () () 0 0)
    (test "expected seven") 30))
```

Its report is `(core 0.1 (failed "got six" (effects () () ())))` (exit 2).
Changing fuel to 0 gives `(core 0.1 (outOfFuel))` (exit 3). An ordinary
function with `fail` instead is `illTyped` (exit 5), never evaluated.

The exporter below writes all twelve real programs with their timing worlds
and argument values. Wc's run returns the **Core value** `1` but the oracle
process exits 0, with the three counted rows and missing-file error in its
effects. Account's returns
`Ok((Account(owner: "alice", balance: 95), Account(owner: "bob", balance: 25)))`.

## Coverage and timings

`TextChecks.lean` uses `#guard` for both codec directions on all twelve
programs, their full runs, every primitive family, every Value constructor,
World, all internal Outcome constructors, reports and malformed lexical
inputs. Equality checks compare the original and decoded Lean `Repr` as
well as canonical output; NaNs are checked by bits, not IEEE equality.
Existing Builtins expression cases also round-trip before execution.
No inversion theorem is claimed. `ci/test_mocore.sh`, run by `make formal`,
tests actual file input, UTF-8 rejection, exits and retained stop effects.

Measured 2026-10-12 in this Linux x86-64 orb, Lean 4.34.0 native executable.
One warm-up, then **21 fresh processes per sample**; wall time includes
process startup, input I/O, parse, checker, initialization, entry evaluation,
structural rendering and captured stdout. Fuel 10000. These are the inputs
in `TextChecks.samples`, not the entire Mo test suite or a VM benchmark.

| Sample | Input bytes | Median ms | Min ms | Max ms |
| --- | ---: | ---: | ---: | ---: |
| Greet | 1115 | 3.733 | 3.079 | 4.505 |
| Wc | 4644 | 3.951 | 3.632 | 4.833 |
| Find | 8694 | 3.911 | 3.473 | 4.586 |
| Expenses | 14229 | 4.461 | 4.171 | 4.996 |
| Json | 25000 | 5.094 | 4.532 | 5.846 |
| Todo | 12178 | 4.210 | 3.812 | 4.913 |
| Account | 6004 | 3.832 | 3.509 | 4.461 |
| RateLimit | 5414 | 3.768 | 3.350 | 4.382 |
| JobQueue | 12553 | 4.026 | 3.529 | 4.597 |
| Kv | 14032 | 4.216 | 3.773 | 4.963 |
| NotesTool | 4535 | 3.484 | 3.090 | 4.601 |
| Inventory | 25063 | 5.652 | 4.630 | 6.219 |

Reproduce from `formal/` (Python is only the timing harness):

```sh
lake env lean --run ExportSamples.lean /tmp/mocore-samples
python3 - <<'PY'
import pathlib, statistics, subprocess, time
exe = '.lake/build/bin/mocore'
for path in sorted(pathlib.Path('/tmp/mocore-samples').glob('*.core')):
    subprocess.run([exe, str(path)], check=True, capture_output=True)
    times = []
    for _ in range(21):
        start = time.perf_counter_ns()
        result = subprocess.run([exe, str(path)], check=True, capture_output=True)
        assert result.stdout.startswith(b'(core 0.1 (returned ')
        times.append((time.perf_counter_ns() - start) / 1e6)
    print(path.stem, path.stat().st_size, statistics.median(times), min(times), max(times))
PY
```

## Extending the contract

The codec describes today's Lean constructors, not primitives planned for
later streams. Fake capability constructors and `Unicode16` are not added
here. The existing `randomInt` and world's natural seed are transported,
but the current model uses a placeholder generator, **not** the architecture's
xoshiro256**/SplitMix64/Lemire oracle; don't use it to certify VM randomness.

To add a primitive, first add its Lean operation constructor, signature,
evaluator and safety proof. Add its exact constructor-named S-expression
with explicit fields to both codec directions, the table/schema here and
the guards (including a case not already covered by samples). Fake worlds
or actual PRNG state will also require explicit Value/World changes and
new run/report examples. Do not silently reinterpret the existing natural
seed, Boolean `ro`, root, or a constructor's field order.

Any added constructor or changed shape/meaning advances the minor wire
version (next `0.2`), updates both Lean and C and their differential corpus,
and adds tests for rejecting unsupported versions. The current parser
accepts **only `0.1`** and rejects unknown constructors; there are no ignored
extension fields or fallback operation names. Support for older versions,
if wanted, must be explicit. This document, not the informal lowering
notation or Lean's `Repr`, specifies what the C implementation must write.
