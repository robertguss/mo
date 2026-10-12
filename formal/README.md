# Mo Core

Mo Core is the small internal language that every feature of the Mo language
(the Mo Language Reference v0.1, `docs/reference/README.md`) lowers to. It is
the contract between Mo's front end and its back end (architecture, section
4), and this Lean 4 development is its definition: syntax, typing rules and
an executable evaluator, with proofs.

Core is a **typed, monomorphic, strict functional language**:

- **Types**: `Int` (64-bit, checked), `BigInt`, `Byte` (0 to 255), `Decimal`
  (an integer count of 10⁻¹⁸ within ±(2¹²⁷ − 1)), `Float`, `Bool`, `Char`,
  `Text`, `Bytes`, `Unit`, `Range` (`a..b`, two `Int`s), `Time` and
  `Duration` (whole microseconds), tuples, `List`, `Map` and `Set` (both
  insertion-ordered; a set holds no element twice), declared data types
  (records and choice types share one form, looked up by name in the
  program's data table), the library's `Option` (`None`, then `Some`, as
  reference 8 declares it), `Result`, `Ordering`, `FileError`, `Entry`
  (`name: Text`, `kind: EntryKind`) and `EntryKind` (`File`, `Folder`,
  `Link`, `Other`, in that order), function
  types, and the capability types `Console`, `Files`, `ReadFiles`, `Clock`,
  `Random`, `Args` and `Env`. There are no type variables and no abilities:
  generic Mo code is instantiated per type first.
- **Expressions**: literals, variables, `let`, anonymous functions (closures),
  application of a function value, calls of top-level functions, tuples and
  projection, constructor application, `case` on one level of constructors
  (one arm per constructor), `if`, primitive operations (`Prim`, each with a
  typing signature), `stop "reason"`, `fail e` with a `Text` detail, reads of
  module constants, and join points: `join j(params) = body
  in rest` and `jump j(args)`, jumps only in tail position, a join body may
  jump to its own join point. Loops, `break`, `continue`, `return` and `try`
  lower to join points.
- **Programs** have ordered `const name : T = e` declarations, functions
  with a module and kind (`function`, `test`, `example`, `promise`, `anchor`),
  and one designated ordinary entry. Constants are computed once before
  execution, each seeing only its preceding prefix, including through calls.
  The last four function kinds and anonymous functions inside them are spec
  code: only they may use `fail`, `printed`, `printedErrors` and the seven
  fake constructors of reference 12.4. Ordinary code
  cannot call a spec function, including through a named-function wrapper.
  The typing rules and executable checker enforce these restrictions and the
  kind signatures of bytecode 3.3.
- **Effects** go through primitives on capability handles, which read and
  update one `World`: arguments, environment variables, standard input,
  standard output and error, an insertion-ordered in-memory file map, the
  clock's time and xoshiro256**'s four-word state. Real handles use these fields; each fake
  constructor allocates an independent state in `World.fakes`. Handles
  derived by `within` or `read_only` share the same fake state. Observing
  `printed` or `printed_errors` returns that fake console's snapshot, not
  real output. Files hold texts, so `read_bytes` gives a
  file's text as UTF-8. A `Files` or `ReadFiles` handle reaches one folder;
  `within` a path that would leave it gives a handle that reaches nothing, on
  which every call that returns a `Result` gives `Error(NotAllowed(path))` and
  `exists?` gives `false`.
  `entries` returns sorted immediate names and kinds, with folders implied
  by file paths, on both real and fake Files/ReadFiles. Seeded Random uses
  four SplitMix64 outputs and Lemire's unbiased reduction, including the
  widest expressible half-open Int interval.
- **Semantics**: `eval : Nat → Program → Env → World → Expr → Outcome`, a
  big-step evaluator with fuel. An outcome is a value and the new world;
  `stopped` on a bug (overflow, division by zero, `stop`, a library call
  outside what it accepts, a map literal whose keys turn out equal; with the
  world so far); `failed` with its Text detail and world, distinct from a
  stop; an internal `jumped` to a join point; `wrong` (stuck, only
  for ill-typed programs); or `outOfFuel`. Overflow stops; it never wraps.
  Unicode16's five functions have trusted primitive signatures and safety,
  but reached calls stop at an explicit oracle boundary. Runtime Unicode
  tables are checked against UCD 16.0's tests, not against Lean.

`eval` evaluates an expression without constants; `evalWithGlobals` takes
their computed values. `evalProgram` initializes constants before evaluating
an expression, `runProgram` before calling the designated entry with supplied
runtime arguments, and `runTest` before calling a test or example. These are
fresh runs. `initConsts`'s continuation can run several calls with one computed
prefix; multi-call tests retain fake handles in one Core expression. Runners
return Core outcomes, not OS exit statuses; lowering supplies any entry wrapper (11.2).

How each Mo feature lowers to Core is written down in
[TRANSLATION.md](TRANSLATION.md), section by section, with worked examples.

## Building

```sh
cd formal
lake build
```

The project uses Lean 4.34.0 (`lean-toolchain` names
`leanprover/lean4:v4.34.0`) and nothing but Lean's bundled library. A clean
build takes about half a minute. It checks every proof, runs the checker on
all twelve sample programs inside the kernel, and evaluates every example and
test, and the checks of `MoCore/Builtins.lean`, with `#guard`, so a wrong
result fails the build.

`lake build` also builds `mocore`, the native differential-test oracle.
`lake exe mocore run.core` reads a versioned Core program and world, checks
the program, runs its entry or a selected test/example, and prints one
observable report. [TEXT.md](TEXT.md) specifies the complete wire grammar,
canonical printing, result protocol, extension rules and sample timings.
`make formal` additionally runs the executable's file/CLI checks.

## Layout

| File | What it holds |
| --- | --- |
| `MoCore.lean` | The root: imports everything. |
| `MoCore/Syntax.lean` | Types (`Ty`, with decidable equality), literals, the primitive operations and their signatures (`Prim.sig`), expressions (`Expr`), declarations and programs, and the lookups (`ctorsOf`, `lookupFun`, ...). |
| `MoCore/Metadata.lean` | Compiler side-table traversal and shape checks, separate from typing/evaluation. Program carries metadata, but no semantic rule or proof reads it. |
| `MoCore/Value.lean` | Values (real/fake handles), environments, `CapState`, the `World` and its fake-state store, bugs, outcomes, and the number ranges. |
| `MoCore/Support/Num.lean` | Exact arithmetic: rounding (ties to even, ties away from zero), decimal multiplication and division, wrapping and saturating, decimal and whole-number text forms and parsers. |
| `MoCore/Support/Text.lean` | The text functions of reference 14 (`lines`, `words`, `split`, `trim`, ...), over lists of characters. |
| `MoCore/Support/Time.lean` | Calendar conversions and RFC 3339 text for times; durations' text form. |
| `MoCore/Support/Random.lean` | SplitMix64 seeding, xoshiro256** 1.0 draws and Lemire's full-width multiply-high rejection. |
| `MoCore/Support/Bytes.lean` | UTF-8 over lists of bytes: encoding text (`Text.bytes`) and decoding it (`Bytes.decode_utf8`, by table 3-7 of the Unicode Standard); the order of bytes. |
| `MoCore/Support/Equiv.lean` | Structural equality of values (maps and sets in any order, floats by IEEE), `Distinct` (no element of a list equals one after it, as in a set), and the built-in orders. |
| `MoCore/Prim.lean` | What every primitive does: `Prim.eval`, with the range checks. |
| `MoCore/Eval.lean` | Expression evaluators, `initConsts`, `evalProgram`, `runProgram`, `runTest`, and `run`/`holds`. |
| `MoCore/Typing.lean` | The typing judgment `Typed`, well-typed programs `WellTyped`, value typing `ValTy`, `OutcomeOk`, `PrimSafe`. |
| `MoCore/Safety.lean` | Type safety of the calculus, given safe primitives: `eval_safe`. |
| `MoCore/PrimSafety.lean` | Every primitive respects its signature: `prims_safe`. |
| `MoCore/Theorems.lean` | The unconditional theorems: expression, initialization and runner safety, the corollaries, determinism. |
| `MoCore/Check.lean` | An executable type checker, `tc`, and its soundness: `tc_sound`, `checkProgram_sound`. |
| `MoCore/Fuel.lean` | Fuel is only a bound, for expressions, initialization and both runners. |
| `MoCore/DSL.lean` | Lean helpers for writing Core programs readably, named after the Mo constructs they lower. |
| `MoCore/Lib.lean` | Library functions written in Core (`map`, `filter`, `sort_by_desc`, `update`, ...), as generators instantiated per type, the order they sort by (`Lib.lt`, in which a float that is "not a number" comes last), and a set's text form (`setText`, `[1, 2].to_set`). |
| `MoCore/Testing.lean` | `passes` for checking examples and tests, and `stops` for checking that an expression stops on a bug; lemmas turning a checked program into "never goes wrong" theorems. |
| `MoCore/Builtins.lean` | Checks of built-in types, Unicode scalar boundaries, failure, programs and fakes; independent Blackman/Vigna random vectors, Lemire rejection/full-width intervals, Unicode16 interfaces and real/fake folder entries. |
| `MoCore/Examples/*.lean` | The twelve samples of `examples/`, each in Core, with every example line and test of the sample as a `#guard`, and theorems that the program is well typed and never goes wrong. |
| `MoCore/Text/*.lean` | The reference text codec: S-expressions, types/literals, primitives, programs, lossless values/worlds/outcomes, structural observations and checked run requests. |
| `MoCore/TextChecks.lean` | Round trips for all twelve programs, every primitive/value, Builtins programs, reproducible runs and report/lexical checks. |
| `Main.lean` | The `mocore` file/CLI protocol, with distinct diagnostic and outcome exit codes. |
| `ExportSamples.lean` | Writes twelve entry runs plus Fakes, Library and synthetic Metadata, in Core text 0.5; optional second directory exports all fourteen fuzz seeds (`lake env lean --run ExportSamples.lean DIR FUZZ_DIR`). |
| `TEXT.md` | Complete Core text 0.5 contract shared by Lean and C, compiler metadata, worked runs and measurements. |
| `TRANSLATION.md` | The lowering from every v0.1 feature to Core, how the model follows the answers D227 gave to the questions writing it raised, the three things D228 added, and what the model decided in making `Byte`, `Bytes`, `Range` and `Set` types of their own. |

## The twelve programs

Each file in `MoCore/Examples/` translates its sample by hand, following
TRANSLATION.md; each Core function's comment shows the Mo source it comes
from. Every example line (reference 12.1) and test (12.3) of the sample is a
condition lowered to a Unit assertion that `#guard` checks is well typed in
spec code and returns normally; promise anchors (12.2) are checked too.
Greet's examples use named example functions and `runTest`; its entry also
runs through `runProgram`. Promises have kind `promise` and return 0 on
success or the first false line's one-based number, as bytecode 3.3 requires.

| Program | What its checks cover |
| --- | --- |
| `Greet` | `greeting` with `Some`/`None`; `main` with `Args.of(["Ada", "Grace"])` and a fake console prints `["Hello, Ada!"]`. |
| `Wc` | `count` (including `count("one two\nthree\n") == Counts(lines: 2, words: 3, bytes: 14)`), `row`; `main` over three files, one missing: status 1, the three printed rows, and `wc: gone.txt: no such file`. |
| `Find` | `parse_args` (five examples), `matches`; `main` with `-i`, with no match (status 1) and a missing file (status 2); also `--count`, an unknown option, and lines that end in `\r\n` or keep a lone `\r`. |
| `Expenses` | `parse_line` (five), `totals` (`[food(1.50), rent(4), food(3)]` gives `[("food", 4.50), ("rent", 4)]`), `layout`; `main` on a CSV with a malformed line. |
| `JsonQuery` | the whole parser of `examples/json-query.mo`, module JsonQuery: `parse` (five examples, including `é`), `hex_value`, `pretty`, `quoted`, `at_path` (three); `main` answering `items.0.name`; a pretty-printed document and a parse error through `main`. |
| `Todo` | `parse_command` (five), `parse_items`, `format`, `apply` (three); the promise's anchors; `main` run four times against one in-memory file system, then `files.read_text` and the listing. |
| `Account` | `deposit` and `withdraw` (four each, including `withdraw(alice(100.00), 100.01) == Error(InsufficientFunds(short_by: 0.01))`), `transfer` (two); all four promises' anchors, and the promises on more inputs; both tests. |
| `RateLimit` | `new_limiter` (three); the request sequence of both tests, at `t0`, then `t0 + 500.milliseconds` (500000 µs); the promise's anchors; a longer sequence with a slow refill; also a time minus a duration: a bucket refilled from `t0 - 1.hours`, the bug before the year 1, and `checked_add` with a negative duration. |
| `JobQueue` | `lease` (two), `fail` (four), `expire` (two); two promises' anchors; the three-attempts test with a range loop and assertions. |
| `Kv` | `parse` (seven), `increment` (four, including the overflow at `9223372036854775807`), `expire` (four); the promise's anchors; key expiry; the console session. |
| `NotesTool` | `search_notes` over in-memory files: only `.md` notes, case ignored; the limit and `truncated?`; typed errors; the promise's anchors; a `within("notes")` capability that sees only its folder, a `within("..")` one that reaches nothing, and paths with `..` inside and outside the folder. |
| `Inventory` | the generic `top_by`, `count_by`, `first_by` and `table`, instantiated at the types the sample uses (`top_by[Int,Int]`, `first_by[Text,Option(Char)]`, ...): their nine examples and two tests, and `main`'s whole output; also a map literal whose keys turn out equal (a bug), and `to_map` with a repeated key. |

## What is proven

Everything below is fully proven: there is no `sorry` anywhere.

**Type safety** (`MoCore/Theorems.lean`):

- `type_safety`: `WellTyped P → Typed P Γ [] e τ 0 spec → EnvTy P ρ Γ →
  OutcomeOk P [] τ (eval n P ρ w e)`, for every fuel `n` and world `w`. In
  words: in a well-typed program, a well-typed expression evaluated in an
  environment whose values have the context's types never goes `wrong`; a
  value it returns has its type; and it never ends with a stray jump. Stopping,
  assertion failure and exhaustion of fuel are safe terminal outcomes.
- `initConsts_safe`: initialization preserves a typed prefix, including
  captured prefixes in closures, and any safe continuation.
- `program_type_safety`: the same safety guarantee for `evalProgram`, with
  expressions typed at the full constant prefix and either spec flag.
- `runProgram_safe`, `runTest_safe`: the entry with typed runtime arguments,
  and a selected test or example, have safe outcomes after initialization.
- `eval_never_wrong`: under the same hypotheses, `eval n P ρ w e ≠ .wrong msg`.
- `eval_ok_typed`: if `eval n P ρ w e = .ok v w'` then `ValTy P v τ`.
- `eval_no_stray_jump`: `eval n P ρ w e ≠ .jumped j vs w'`.
- `int_result_in_range`, `byte_result_in_range` and
  `decimal_result_in_range`: an `Int` result is within the 64-bit range, a
  `Byte` result from 0 to 255 and a `Decimal` result within ±(2¹²⁷ − 1),
  because value typing includes the ranges: overflow can only stop.
- `set_result_distinct`: a `Set` result holds no element twice: no element
  equals (`==`) one after it, which value typing requires of every set, and
  every set primitive keeps.
- `eval_deterministic`: the evaluator is a function, so equal inputs give
  equal outcomes.

**The calculus, from a hypothesis on primitives** (`MoCore/Safety.lean`):

- `eval_safe`: `WellTyped P → PrimsSafe P → ∀ n, EvSafe P (evalWithGlobals n P)`, where
  `PrimsSafe P` says every primitive, given arguments of its argument types,
  returns a value of its result type or stops. The proof is by induction on
  the fuel, with one lemma per helper of the evaluator: `evalArgs_safe`,
  `applyClosure_safe`, `callFun_safe`, `project_safe`, `findArm_typed`,
  `selectArm_safe`, `joinLoop_safe`, `catchJump_safe`, `OutcomeOk.bind`.

**Every primitive respects its signature** (`MoCore/PrimSafety.lean`):

- `prims_safe : PrimsSafe P`, for every program, from one theorem per group:
  `intOp_safe` (all 32 `Int` operations, including checked, wrapping and
  saturating arithmetic, conversions, `to_byte` among them, and durations),
  `bigOp_safe` (14), `byteOp_safe` (2), `decOp_safe` (all 16 `Decimal`
  operations), `floatOp_safe` (15), `not_safe`, `cmp_safe` (`<`, `<=`, `>`,
  `>=` at all eleven base types), `charOp_safe` (7), `textOp_safe` (all 28,
  `bytes` included), `bytesOp_safe` (3), `rangeOp_safe` (all 6 `Range`
  operations), `timeOp_safe` (all 13 `Time` and `Duration` operations),
  `listOp_safe` (all 19 list operations, at every element type), `mapOp_safe`
  (all 12, map literals included, at every key and value type), `setOp_safe`
  (all 11 set operations, at every element type), `eq_safe` (structural
  equality at every type), the trusted stop for all five Unicode16 operations,
  and `ioOp_safe` (all 26 capability operations,
  32 counting the `Files` and `ReadFiles` forms apart), with dispatch to real
  or fake state and value typing for both kinds of handles. So every
  primitive is proven safe, and `type_safety` has no hypothesis on
  primitives.

**The checker is sound** (`MoCore/Check.lean`):

- `tc_sound`: whatever the executable checker `tc` accepts is well typed
  (`Typed`), at the expected type when one is given.
- `checkProgram_sound`: `wellTyped? P = true → WellTyped P`.
- `checked_never_wrong`: a program and an expression the checker accepts never
  go wrong, by `tc_sound` and `type_safety`.

**Fuel is only a bound** (`MoCore/Fuel.lean`):

- `eval_fuel_mono`: `eval n P ρ w e ≠ .outOfFuel → n ≤ m →
  eval m P ρ w e = eval n P ρ w e`. An answer the evaluator gives doesn't
  depend on how much fuel it was given, so differential tests of the VM
  against this model are sound.
- `evalWithGlobals_fuel_mono`, `initConsts_fuel_mono`,
  `evalProgram_fuel_mono`, `runProgram_fuel_mono`, `runTest_fuel_mono`:
  the same guarantee with computed globals, across initialization, and for
  whole runs, including failed or stopped ones.

**The twelve programs are well typed and never go wrong**
(`MoCore/Examples/*.lean`), each by the kernel running the checker
(`decide +kernel`, which adds no axiom):

- `checked : wellTyped? program = true` and `wellTyped : WellTyped program`
  in every sample.
- `main_never_wrong` for `Greet`, `Wc`, `Find`, `Expenses`, `JsonQuery`, `Todo`,
  `Kv` and `Inventory`: running `main` with its capability handles, in any
  world (any arguments, input, files, time) and with any fuel, never goes
  `wrong`. For the samples without a `main`: `transfer_never_wrong`
  (`Account`), `request_never_wrong` (`RateLimit`), `lease_never_wrong`
  (`JobQueue`), `search_never_wrong` (`NotesTool`), each for all well-typed
  arguments, and `parse_never_wrong` (`JsonQuery`) for every text.

Supporting lemmas include `Ty.eq_of_beq` and `Ty.beq_refl` (decidable
equality of types), `EnvTy.lookup`, `EnvTy.bindParams`, `EnvTy.bindVars`,
`ValsTy.getElem?`, and the value-typing inversions `valTy_*_iff`.

## Not yet proven

No theorem is left with `sorry`. What the proofs don't yet cover:

- **The lowering itself.** TRANSLATION.md is a written specification, and the
  twelve programs are translated by hand; there is no formal Mo syntax or
  proof that lowering preserves meaning.
- **Functional correctness of primitives.** The primitives are proven to
  respect their types and ranges, not to compute what reference 14 says (for
  example, that `decMul` rounds ties to even). The examples test that, and
  `MoCore/Builtins.lean` checks UTF-8 decoding against Lean's own decoder on
  every sequence of one or two bytes and on large families of three and four.
  It also checks seeded words and draws against the compiled published
  SplitMix64/xoshiro256** reference code and bounded results against Lemire's
  C sample. Period, statistical properties and functional correctness are
  not proved; Unicode16 conformance belongs to runtime UCD testing.
- **Floats** have no proofs beyond their types. Their text form writes a
  float that is not a number as `nan` and the infinities as `inf` and
  `-inf`, as reference 9.2 spells them (checked in `MoCore/Lib.lean`), but
  finite floats now use the shortest round-tripping decimal shared with
  `to_decimal`. Exact rational-to-binary64 conversion rounds once, ties to
  even, including subnormals and overflow; midpoint and double-rounding
  regressions are checked by bits. Functional correctness is not proved.
  The VM's float text form is a trusted primitive, checked against Ryu
  (architecture 4.5).
- **Completeness of the checker** (that it accepts every well-typed
  expression) is not proven; it is sound, and accepts all twelve programs.
- **Progress for programs that don't terminate**: the evaluator returns
  `outOfFuel` for them; there is no coinductive semantics.

Not modeled yet: Unicode16's tables, the text forms the lowering derives for records, choice
types, tuples, lists and maps, and the quoted form of a text or a character
inside another value (`to_text[T]`, which both `"#{x}"` and `x.to_text` call,
TRANSLATION.md 3.4; a set's is `Lib.setText`, given how its elements are
written). Random now implements architecture 7's actual generator and range
reduction. Real OS links/special files cannot appear in the model's text-file
map; their EntryKind values are typed and transportable. `walk` remains Mo
library code, not a Core primitive. The interchange text and structural oracle observations
are now defined in TEXT.md; they do not implement the Mo `to_text[T]`
functions inside the calculus. `Byte`, `Bytes`, `Range` and `Set`
are types of their own (TRANSLATION.md 3.2, 3.4, 7.5 and 3.8, and the
decisions in TRANSLATION.md). Failure, the program layer and independent
fake capabilities, folder entries and Unicode16's interface are now modeled;
their decisions and the remaining design/reference mismatches are recorded
in TRANSLATION.md's final sections.

## Axioms

The proofs use only Lean's standard axioms, `propext`, `Classical.choice` and
`Quot.sound` (`#print axioms type_safety` lists exactly these). There is no
`sorry`, no `native_decide`, and no `axiom` declaration.
