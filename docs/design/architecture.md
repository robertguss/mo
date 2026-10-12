# Mo Architecture v0.1

**Draft, 12 October 2026, revised after two independent reviews.** This is how
Phase 2 builds the `mo` toolchain in C (decision D207): the compiler, the
virtual machine, and the contracts between them. It is written against the
Language Reference v0.1 (`docs/reference/`), whose rules win wherever the two
disagree. It also builds on Mo Core (`formal/`) and the bytecode
specification (`bytecode.md`). It fixes what Phase 2's work streams build
against, so they can work in parallel. Section 13 lists what is still open.

## 1. The shape of the system

One program, `mo`, with the commands `mo check`, `mo run`, `mo test`,
`mo format` and `mo fix`. It compiles a whole program at once, from source to
bytecode, and runs the bytecode on its own virtual machine (D195).

```text
┌────────┐  ┌────────┐  ┌──────────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────┐
│ source ├─►│ parser ├─►│ names, types ├─►│ lowering ├─►│ bytecode ├─►│ loader,  ├─►│  VM  │
│ (.mo)  │  │        │  │  and checks  │  │ to Core  │  │ compiler │  │ verifier │  │      │
└────────┘  └────────┘  └──────────────┘  └────┬─────┘  └──────────┘  └──────────┘  └──┬───┘
                                               │ Core, as text                         │ outcomes
                                               ▼                                       ▼
                                        ┌──────────────┐                        ┌──────────────┐
                                        │ Lean         │◄──── compared ────────►│ value, stop  │
                                        │ evaluator    │                        │ or failure   │
                                        └──────────────┘                        └──────────────┘
```

- **The front end** turns source into Mo Core. It parses, resolves every name,
  types every expression and checks every rule of the reference, and then
  lowers the checked program to Core and instantiates its generic functions.
- **Mo Core** is the small language every feature lowers to. It is defined in
  Lean, which gives it an evaluator to test against (section 4). It has a text
  form, so the C toolchain and Lean can exchange programs.
- **The bytecode compiler** turns Core into bytecode. It places the reference
  counting, the borrowing and the in-place reuse.
- **The VM** loads bytecode, verifies it (condition 5 of D207), and runs it. It
  never sees source. Everything it does to the outside world goes through
  capability objects (section 7).

Four contracts connect the parts, and each one is a file that every side reads:

| Contract | Defined in | Between |
| --- | --- | --- |
| Mo Core | `formal/` (Lean) and `src/core/core.h`, with a text form | the front end, the bytecode compiler and the Lean model |
| The bytecode format | `bytecode.md` and `src/bytecode/bytecode.h` | the bytecode compiler and the VM |
| The primitives | `src/base/prims.def` | the checker, the compiler, the verifier and the VM |
| The capability operations | `src/base/hostops.def` | the same four, and the host |

## 2. C modules and their boundaries

```text
src/base/      assertions, arenas, limits, checked arithmetic (64 and 128 bits,
               and Decimal's 256-bit products), UTF-8, hashing, interned names,
               diagnostics, source files, and the tables prims.def and hostops.def
src/syntax/    lexer, parser, syntax tree, formatter
src/check/     program loading, names, types, abilities, capabilities,
               exhaustiveness, the rules of reference section 6
src/lower/     lowering the checked program to Core; monomorphization
src/core/      Core's data structures, its text form, and the Core checker
src/codegen/   borrow inference, reference counting, reuse, registers; writes bytecode
src/bytecode/  the format: writer, reader, verifier, assembler and disassembler
src/heap/      the runtime type table and layouts (types.h); values, cells, heap
               objects, reference counts, and the generic equality, hashing,
               dropping and printing walks
src/prim/      the primitives of prims.def
src/host/      capabilities, real and fake
src/vm/        processes, the interpreter, calls and frames, stops and defect reports
src/tools/     the mo command: check, run, test, format and fix; diagnostics as text and JSON
lib/           the standard library written in Mo (condition 4 of D207)
```

Every module uses `base`. Beyond that, each uses only the modules listed here:

| Module | Uses |
| --- | --- |
| `syntax` | none |
| `check` | `syntax` |
| `lower` | `check`, `core` |
| `core` | none |
| `codegen` | `core`, `bytecode`, and `heap`'s types |
| `bytecode` | `heap`'s types |
| `heap` | none |
| `prim`, `host` | `heap` |
| `vm` | `bytecode`, `heap`, `prim`, `host` |
| `tools` | all of them |

The VM never sees source, syntax trees or Core. Each module has one public
header, named after it, such as `src/check/check.h`. There are two
exceptions. `base`'s headers are each a separate facility: `base.h`,
`arena.h`, `buffer.h`, `limits.h`, `diag.h`, `codes.h`, `prims.def` and
`hostops.def`. And `heap` has
a second header, `types.h`, with the runtime type table, the layouts and
`mo_case_shape`; the compiler, the bytecode format, the primitives and the
host use it without the rest of the heap. Nothing else is included across
modules.

Every module follows the five conditions of D207: sanitizers on every test,
fuzzing in CI, Tiger Style (`MO_ASSERT` always on, explicit limits, arenas,
every result checked, `-Wconversion`), a small trusted C core, and bytecode
verified on load.

**Memory.** Compiler stages allocate from arenas that live as long as the
stage's output and are freed at once. The VM allocates Mo values from a
process's heap, with reference counts (section 6). No module calls `malloc`
directly outside `base` and `heap`.

**Limits.** Every input has a named limit in `src/base/limits.h`, so a test can
reach each one. Limits on what the compiler reads are **diagnostics**: file
size, line length, nesting depth, names per module, registers per function,
instructions per function. Limits on a running program are **bugs**, with a
report (reference 13.2): the stack's size, the call depth and the heap's size.

**128-bit integers.** `base.h` defines `mo_i128` and `mo_u128` as
`__extension__ typedef __int128`. GCC and Clang both have the type. The
`__extension__` keeps `-Wpedantic` quiet, and a static assertion fails the
build on a compiler without it. Decimal's 256-bit intermediate products are
written once, in `base`, with CBMC proofs and exhaustive tests against a
big-number reference.

## 3. The front end

### 3.1 Parsing and formatting

The parser is hand-written recursive descent, because Mo's error messages need
to know which block is unfinished and where to resume (D33, D223). It produces
a syntax tree whose every node has a span: a file, and start and end byte
offsets. Lines and columns are computed when a message is printed. Columns
count characters, as the executable grammar does.

The executable grammar (`grammar/mo.y`) is the specification. The parser and
the grammar must agree on every input within the limits of
`src/base/limits.h`: they accept the same inputs and reject the same ones. The
grammar enforces none of those limits but the depth of interpolations, so the
fuzz target that checks this continuously, by running both on the same bytes,
compares them only on inputs within the limits. After an error, the parser
resumes at the next line that can start a declaration or a statement, so one
mistake gives one error.

`mo format` prints the syntax tree back. The tree keeps every comment: a
comment on its own line is attached to the next node at its indentation, or
to the end of its block if nothing follows. A comment at the end of a line is
attached to that line's last node. Formatting is a function of the tree.
Formatting twice changes nothing, and the formatted program's tree equals the
original's, except for the rewrites the reference names: `x = x + y` becomes
`x += y` (reference 6.2).

### 3.2 Loading a program

`mo` is given one file: the program's **entry module**, whose `main` `mo run`
runs. The entry module's name, such as `Shop.Cli`, together with its path,
such as `app/shop/cli.mo`, gives the **root folder** (`app/`). Every other module is found
from its name under the root (reference 2.1). The loader reads each file's
header, the `module` line and the `use` lines, with `mo_read_header`, without
parsing the rest. It follows `use` lines to the files they name, and builds
the module graph, refusing cycles. The library's modules (`lib/`) are found
in the toolchain's own folder, and are always available.

### 3.3 Names and types

Type checking is **bidirectional**, one function body at a time. A signature
is given, and the body is checked against it: an expression is either
checked against an expected type or has its type inferred, and inferred types
are unified. Inference never crosses a function boundary, so errors stay
local. Names are resolved during checking, not before it, because Mo resolves
some names by type:

- **A call** checks its callee first. It then checks each argument against the
  callee's parameter type. So an anonymous function passed as an argument
  learns its parameter types before its body is checked, as in
  `customers.filter(fn(c) c.active? end)`.
- **A dot call** `x.f(...)` is resolved once `x`'s type is known: in the module
  that declares that type first, then among the names in scope (reference
  5.4). A field of `x`'s record wins where it's visible. When the field holds a
  function, `x.f(a)` calls it. It's enough to know the type's constructor:
  `var items = []` followed by `items.append(x)` finds `append` in `List`,
  even before the element type is known. If `x`'s type is still unknown when
  the body's other constraints have been solved, that's an error: give `x` a
  type, or write the call another way.
- **Two functions with one name** in a module differ in their first
  parameter's type (reference 5.1). A call picks the one whose first parameter
  matches the first argument's type. If that type isn't known yet, the call
  waits, as a dot call does.
- **Literals** take the type their context needs (reference 3.3). An integer
  literal can be an `Int`, `BigInt`, `Byte`, `Decimal` or `Float`. A literal
  with a decimal point can be a `Decimal` or a `Float`. A literal out of its
  type's range is an error: an `Int` beyond 64 bits, a `Byte` beyond 255, a
  `Decimal` with more than 18 places or beyond its range. So is a `Float`
  literal that doesn't round to a finite value. A literal with nothing to
  decide its type is an `Int`, or a `Decimal`.
- **`is` patterns** bind names where the test is known to be true: in the
  block it guards, after `and`, after `implies`, and after `assert`
  (reference 7.4). The checker tracks this through `and`, `implies` and
  `assert`.
- **Generic functions** are checked once, with their type parameters abstract.
  Every instantiation is compiled separately (section 4.2), so a generic
  function can't reach itself again at a bigger type, which is called
  polymorphic recursion: it would need infinitely many copies. The rule is
  checked over each group of generic functions that call each other, directly,
  through other functions, or through anonymous functions. Within such a group, every
  call to a member must pass type arguments that are the caller's own type
  parameters, never types built from them, such as `List(T)`. Breaking it is
  an error whose message names the calls in the cycle (reference 5.5). As a
  second line of defense, lowering stops with a diagnostic if instantiation
  ever nests deeper than a limit.

### 3.4 Abilities, capabilities and the other rules

- **Abilities** are constraints on type parameters, checked at each call. The
  checker knows which built-in types have each of the five abilities (the
  table in reference 9.2), how records and choice types derive them, and the
  customizing functions `compare` and `to_text`.
- **Anonymous functions** may read a `var` only if nothing assigns it after
  the function is made, on any path, including a later turn of a loop around
  the function (reference 5.6). The checker finds the assignments that can
  follow the function's creation in the function's control flow.
- **Capabilities.** A type *holds a capability* when it is a capability or
  contains one. The checker refuses anonymous functions that capture such
  values, and fakes, `printed` and `printed_errors` outside tests, examples
  and promises (reference 11.4, 12.4).
- **Map literals** whose keys the checker can see are equal, such as two equal
  literals or the same constant twice, are refused (reference 3.8).
- **Exhaustiveness** uses the pattern-matrix method (Maranget, "Warnings for
  pattern matching", 2007). Reference 7.2's rule against whole-arm catch-alls
  over a choice type is checked on top of it.
- **Constants** may refer to each other in any order within the program, and
  are computed in the order of their dependencies. A cycle is an error
  (reference 10.4).
- **Promises and examples** are checked like functions returning `Bool`. An
  example line or a promise body becomes a spec function (`bytecode.md`
  section 3.3). `mo check` evaluates the anchors by compiling them and calling
  the promise's body on the VM, each in a fresh process, with `mo_call`
  (section 8). Anchors may make fakes, as `Files.in_memory({}).read_only`.

### 3.5 Diagnostics

Every stage reports into one structure: a stable code (`E0412`), a headline in
the reference's words, a reason, the section the reason cites, a primary span
with a label, secondary spans with labels, and fixes (D223). The reason is one
sentence that says why, citing the rule's section, as appendix A's "Mo never
drops a failure silently (reference 8.3)." Only the codes of limits, which cite
no section, may have none. A fix is one or more text edits, applied together or
not at all, each against the original text, so they don't overlap; a
diagnostic's fixes are alternatives. `src/tools` renders it
as the text of reference appendix A, or as JSON for `mo check --json`. Codes
are registered in one table, `src/base/codes.h`, with the reference section
each cites. A code is never reused for a different meaning. Diagnostics are
sorted by position. A stage that reports an error passes nothing it can't
vouch for to the next, so one mistake doesn't cascade. `mo fix` applies a
diagnostic's fix when it has exactly one (reference appendix A).

## 4. Mo Core

### 4.1 What Core is

Core is a small, typed, monomorphic, strict functional language. Its syntax,
typing rules and evaluator are defined in Lean in `formal/`, which is the
authority. Its proofs say that well-typed Core programs don't go wrong: they
return a value of their type, stop with a bug, or run out of fuel. This
section summarizes it; `formal/README.md` lists what is proven.

- **Types:** `Int`, `BigInt`, `Byte`, `Decimal`, `Float`, `Bool`, `Char`, `Text`,
  `Bytes`, `Unit`, `Range`, `Time`, `Duration`, tuples, `List`, `Map`, `Set`,
  declared data types (records and choice types share one form: constructors
  with fields), function types and capability types. There are no type
  variables.
- **Expressions:** literals; variables; `let`; anonymous functions and their
  application; calls to top-level functions; tuples; constructor
  application; `case` on one level of a data type's constructors; `if`;
  primitive operations, each checking its own bugs, including the capability
  operations and the fake constructors; `stop`; `fail e`, an assertion's
  failure with the detail `e`, a `Text`, allowed only in spec code; and join
  points: local continuations, called only in tail position, which carry loops
  and early exits.
- **Programs:** the data types; the module constants, each `const name : T =
  e`; and the functions. Each function has its module and a kind: `function`,
  `test`, `example`, `promise` or `anchor`, with the signatures of
  `bytecode.md` section 3.3. The last four are spec code, and so are the
  anonymous functions inside them. One function is the entry (4.3).

This program layer is what the bytecode compiler needs beyond expressions:
kinds become the bytecode's kinds, constants become `init` functions, and the
compiler never has to look back at the checked program. Stream 0 has given
the model in `formal/` `Byte`, `Bytes`, `Set` and `Range` as types of their
own. It still lacks `fail`, the fake constructors, the `Unicode16` module, and
the program layer: its constants are functions without parameters, and it has
no kinds. Stream 0 adds them, and the text form of 4.4 (section 12). The Lean
evaluator reads kinds but needs none of them to run a program.

### 4.2 Monomorphization

Lowering instantiates every generic function at each list of types it is used
with, so Core has no type variables and no abilities. An ability becomes the
specific function for its type: a primitive, a library function, or a
function lowering generates (4.3). Polymorphic recursion is refused by the
checker (3.3), so instantiation always ends. An instantiation belongs to the
module that asked for it, which matters for Phase 5's upgrades (section 9).

### 4.3 Lowering

Each rule here has a written lowering in `formal/TRANSLATION.md`, with
examples. The ones a compiler is most likely to get wrong are:

- **`var`, loops, `break`, `continue` and `return`** become join points whose
  parameters are the `var`s the loop changes. An anonymous function captures
  the value a name has when the function is made. A function can't assign a
  `var` (reference 5.6), so a captured value can't change behind it.
- **`try`** becomes a `case`. On the failure path, the error value is wrapped
  in the enclosing function's error type when the reference says it is
  (reference 8.3): through the failure type's one case with a single unnamed
  value of that type. The checker reports an error when there is no such
  case, or more than one.
- **Patterns** become decision trees of one-level `case`s. `is` becomes a
  `case` whose bindings are visible where 3.3 says.
- **`and`, `or` and `implies`** on `Bool` become `if`. **`or` on an `Option`**
  is lazy too: `x or default` becomes `case x of Some(v) → v | None →
  default`, so `Time.parse(t) or stop "..."` stops only on `None`. The
  checker picks the meaning of `or` from its left side's type.
- **Text interpolation** becomes calls to the values' `to_text` functions and
  concatenation. Lowering generates a `to_text` for each type that has the
  `Text` ability, following reference 9.2: a type's own `to_text` where it has
  one, and otherwise the form that looks like code. A `Text` or `Char` inside
  another value is quoted and escaped, and at the top level it isn't.
  Lowering also generates `compare` for each type with `Order`, using the
  type's own `compare` where it has one. Equality and hashing are structural
  and done by the VM (`bytecode.md` section 8).
- **Ranges** `a..b` are values of type `Range`: two `Int`s. `for i in r`
  becomes a join point that counts from `r`'s start while below its end, so
  the counter never overflows.
- **List and map literals** compute their elements, or every key and value,
  in order, then build the value one entry at a time: a list from
  `List.%empty` with `append`, and a map from `Map.%empty` with
  `Map.%literal_insert`, which stops if its key is already there (reference
  3.8). `to_map` is a different primitive, in which the later value wins.
  A name that starts with `%` is a primitive only lowering emits
  (`src/base/prims.def`).
- **Constants** become one init function each, run in the order of 3.4.
- **`main`** is wrapped in an entry function, which receives the capabilities
  `main` declares and turns `main`'s result into an exit status: `Unit` gives
  0; an `Int` is the status, and one outside 0 to 255 is a bug; `Error(e)` of a
  `Result(Unit, E)` prints `e`'s text form to standard error and gives 1
  (reference 11.2).
- **`assert`** in a test, and each example line, become spec code that calls
  the `fail` instruction with the details: both sides of a failed `==`, or
  the value that didn't match a failed `is`.

### 4.4 The text form

Core has a text form, an S-expression for each Lean constructor, defined in
`formal/` and read and printed by both Lean and `src/core`. `mo check
--emit=core` prints a program's Core. This is how a Core program goes from the
C toolchain to the Lean evaluator, and back, and how stream 2 is tested
without stream 1 (section 11). Values have a text form too, for comparing
results: the structural form of reference 9.2, except that a `Float` is
written as its 64 bits in hexadecimal, so the comparison doesn't depend on
how each side spells floats.

### 4.5 What the Lean model covers

The Lean evaluator implements the primitives whose meaning is simple to state
exactly: integers with overflow, `BigInt`, `Decimal` with its rounding,
booleans, characters, text as a sequence of characters, tuples,
constructors, lists, insertion-ordered maps and sets, time and duration
arithmetic. Their results are compared with the VM's in differential tests
(section 11).

The rest are **trusted primitives**, each checked against its own oracle
instead:

| Primitive | Oracle |
| --- | --- |
| `Float` arithmetic and `sqrt` | IEEE 754 itself: the C library's correctly rounded operations |
| `Float`'s shortest text form and parsing | a reference implementation of shortest round-trip printing (Ryu), and `strtod` |
| RFC 3339 times | a table of cases, and a second implementation in Python run in tests |
| UTF-8 decoding of `Bytes` | an exhaustive test over every 1- to 4-byte sequence |
| `Random` | the published reference code of xoshiro256** and SplitMix64, and Lemire's method (section 7) |
| `Unicode16` | the Unicode Character Database 16.0's own test files |

## 5. Bytecode

`bytecode.md` specifies the format, and is the contract between the bytecode
compiler and the VM. In short:

- Every register and field is a 16-byte cell with a static type. Every
  instruction says which of its operands it consumes, and choice values keep
  small payloads inline.
- Functions use register frames on the VM's own stack, with tail calls. A
  closure is an object holding its function and its captured values.
- The verifier proves that a file is well-typed and that its reference counts
  balance. So verified bytecode can't corrupt memory, even if the compiler
  that wrote it is wrong.

## 6. Values, reference counting and reuse

This follows the research brief on reference counting and reuse: precise,
compiler-placed reference counting as in Perceus, reuse as in Lean 4 and
frame-limited reuse, and borrowed parameters as in Lean 4. `bytecode.md`
gives the exact representation; this section gives the reasons.

- **Immediates** are stored in cells directly: numbers, `Bool`, `Char`,
  `Byte`, `Unit`, `Time`, `Duration`, `Range`, `Decimal` (a 128-bit count of
  10⁻¹⁸), and choice values whose cases have no fields.
- **Choice values** with fields keep a tag and an 8-byte payload. A single
  small field is stored inline, so `Some(3)` and `Ok(Unit)` allocate nothing.
- **Heap objects** have an 8-byte header, a 32-bit reference count and a
  32-bit layout. Counts saturate: an object whose count would overflow
  becomes immortal and is never freed.
- **Lists** are arrays with a length and a capacity, so `get` takes constant
  time, and `append` on a list with one holder grows it in place, in amortized
  constant time.
- **Maps and sets** are insertion-ordered hash tables: an array of entries in
  insertion order, and an index table of positions, as in CPython's compact
  dictionaries. A removal leaves a tombstone, and the table is compacted when
  tombstones reach a quarter of the entries. Both keep insertion order
  (reference 3.8). Like lists, a map with one holder changes in place, and a
  shared one is copied first (reference 13.3).
- **Text** is UTF-8 bytes with a byte length, always valid. The character
  count is computed when asked.
- **Reference counting.** The compiler places `dup` and `drop` so that every
  object is freed as soon as nothing can reach it. Parameters are borrowed
  where inference shows the caller keeps them alive (`bytecode.md` 5.4), which
  removes most count updates.
- **Reuse.** When a value is taken apart and a value of the same layout is
  built, the compiler emits `reset` and `reuse`: with one holder, the old
  memory is reused in place; otherwise a new object is allocated. So `var`
  field assignment and `append` in a loop cost what they would with mutable
  data (reference 13.3).
- **No cycles.** Values are immutable; an anonymous function captures values,
  not variables, and can't refer to itself; and the only mutable objects,
  fakes' state, hold only plain data. So no value can reach itself, reference
  counting reclaims everything, and no cycle collector is needed.
- **No recursion in C.** Dropping, equality, hashing and printing use explicit
  work lists. A legal program that builds a million-deep value can't crash
  the VM by dropping it.
- **One thread.** Phase 2's VM runs one process at a time on one thread, so
  counts are never atomic. Phase 3's processes each get their own heap
  (section 9).

## 7. The outside world: capabilities

A capability is a heap object that carries a table of its operations, real or
fake (`bytecode.md` section 7). The VM calls an operation through the object
it is given. It never calls the operating system itself.

- **Real capabilities** are made once, by the runtime, for `main`: exactly
  the ones `main` declares, from the process's arguments, environment and
  start folder (reference 11.2).
- **Fakes** are made by the fake constructors, which only spec code may call:
  `Console.fake()`, `Files.in_memory(...)` and the rest (reference 12.4). A
  fake's state is a mutable object shared by every handle on that fake. So a
  file `main` writes can be read back, and two fakes in one test stay
  separate. That state holds only texts, bytes, lists and maps, which change
  only as programs change values: a test that still holds the map it gave
  `Files.in_memory` keeps seeing the old map (`bytecode.md` section 7).
- **Files.** A real `Files` or `ReadFiles` holds an open handle on its folder.
  Every path is resolved beneath it, one component at a time, with `openat`
  and `O_NOFOLLOW`, or with `openat2` and `RESOLVE_BENEATH` where the system
  has it. Absolute paths, empty components and `..` are refused before
  anything is opened. So symbolic links and races can't lead outside the
  folder. System errors map to `FileError`: a missing file to `NotFound`, a
  refused path or permission to `NotAllowed`, invalid UTF-8 in `read_text` to
  `NotText`, and anything else to `Failed`, with the system's message.
  `list` sorts names by their bytes, which is the order of their characters.
- **Console.** The runtime ignores `SIGPIPE`. Writing to a closed output
  returns `EPIPE`, which is a bug with status 70: "printing to an output that
  is gone" (reference 13.2).
- **Random.** `Random.seeded(n)` and the real `Random` use xoshiro256**,
  seeded through SplitMix64. `int(low, high)` uses Lemire's method, which is
  unbiased. So a seeded sequence is the same on every machine, and stays so
  within a major version.
- **Clock.** The real clock reads the system's real-time clock, in
  microseconds. `Clock.fixed(t)` always returns `t`.

The library is written in Mo wherever it can be (condition 4 of D207). C
implements only what Mo can't express or must be fast: arithmetic and
conversions, text and byte operations, the basic list, map and set
operations, and the capability operations. Every library function that takes
a function, from `map` and `filter` to `sort_by` and `group_by`, is Mo code
in `lib/`. So C never calls back into Mo.

## 8. Running: processes, outcomes and bugs

An **image** is a loaded, verified program. Its **initialization**,
`mo_image_init`, runs the init functions once, in order, before any process
is made. The image keeps the memory they allocate, and every module constant
is immortal and shared by every process (`bytecode.md` section 3.2). A stop
during initialization stops the program before it starts (reference 10.4).
For `mo test` and `mo check`, it fails the file with its defect report.

A **process** is a stack, a heap and the image it runs. `mo_call` runs one
function in a process and returns its **outcome**:

- **returned**, with the result;
- **stopped**, with a defect report;
- or **failed**, from spec code's `fail`, with the failure's details.

A defect report is data: the reason, the span of the instruction that
stopped, the call stack with each frame's function and span, and the stopping
frame's variables in their structural text form: those still readable at that
instruction (`bytecode.md` section 9). `mo_render_defect` turns it
into the text `mo run` prints. When a process ends, its heap is freed all at
once, whatever the counts say. So a stop or a failure in the middle of a
function leaks nothing. Constants live in the image, made immortal, and
every process shares them.

- **`mo run`** initializes the image, makes a process and calls the entry
  function (4.3). It exits with `main`'s status. On a stop, it prints the
  defect report to standard error and exits with status 70. The verifier
  proves that `main` can't reach spec code (`bytecode.md` section 9), so a
  failed assertion can't happen there. If one ever did, it would be treated
  as a stop.
- **`mo test`** and `mo check` run each test, each example line and each
  call of a promise's body in a fresh process. A stop or a failure is
  reported, and the run goes on to the next.

Frames live on the VM's own stack. The stack's size, the call depth and the
heap's size have limits, and exceeding one is a bug with a report, not a
crash. A tail call reuses its frame, so tail calls never grow the stack
(reference 5.3).

## 9. Hooks for later phases

- **Processes (Phase 3):** the VM's state, its heap, stack and current
  function, lives in one `mo_process` structure from the start, so a scheduler
  can run many. Messages between processes copy values, or share them when
  the value is deeply immutable and its count is atomic, as in Lean 4's
  multi-threaded objects.
- **Introspection (Phase 4):** debug information maps every instruction to its
  span and every register to its source name, and every value has a
  structural text form, so a running process can be shown in source terms.
  Every capability operation passes through one dispatch point, where Phase 4
  adds recording for replay.
- **Live upgrades (Phase 5):** the module table lists each module's exposed
  functions and types, and the type table gives what the upgrade checker
  compares. An instantiation of a generic function belongs to the module that
  asked for it (4.2), so swapping a module knows which instantiations to
  rebuild. Calls between modules are direct in Phase 2; Phase 5 adds the
  indirection it needs.
- **In-place demands (Phase 7):** the in-place check of D26 runs on Core, where
  the full-3c checker's rules apply.

## 10. The tools

- **`mo check`** loads, checks and lowers the program, and evaluates the
  promises' anchors. It prints diagnostics as text, or as JSON with `--json`
  (10.2). `--emit=core` and `--emit=bytecode` print the program's Core or
  bytecode in their text forms.
- **`mo run FILE [ARGS...]`** checks the program whose entry module is
  `FILE`, including the anchors, as `mo check` does, then runs its `main`
  with the arguments after the file name. The image it runs is the one the
  anchors ran in. Its spec code is there, but `main` can't reach it.
- **`mo test`** runs every example, every test, and every promise against
  generated inputs, or those that `--only` and `--in` choose by the names of
  `bytecode.md` section 3.5. `--list` prints the names, and `--json` gives
  the results as 10.2 says.
- **`mo format`** and **`mo fix`** rewrite files in place (3.1, 3.5).

**Limits.** `mo run` and `mo test` give each process the limits of
`struct mo_limits` in `src/base/limits.h`, whose defaults are the defaults of
`--max-stack`, `--max-depth` and `--max-heap` (reference 13.4). The VM counts
frames for the depth, so a tail call, which reuses its frame, never counts.
Going past a limit stops the process with a defect report that names the
limit and its value. A program can't set its own limits until Phase 3's
budgets subsume them.

**The `mo` command's own exit status** is 0 when everything passed, 1 when
the program has errors or a test failed, and 2 for a mistake in the command
itself. `mo run` exits with the program's own status once the program runs
(reference 13.4).

### 10.1 Generating inputs for promises

`mo test` checks a promise's body on many generated inputs, boundaries first
(reference 12.2). These are the rules:

- **Boundaries.** Each type has a list of values to try first: for `Int`, 0,
  1, −1 and the two extremes; for `Decimal`, 0, the smallest step, ±1 and the
  extremes; for `Text`, the empty text, one ASCII character, one character
  outside ASCII, and a long text; for lists, maps and sets, empty, one
  element, and several; every case of a choice type; for `Time`, the first and
  last instants of the years 1 and 9999, and the epoch; for `Duration`, 0 and
  the extremes. The boundary inputs are the combinations of the parameters'
  boundaries, up to a limit.
- **Random inputs** follow, from a generator seeded by the function's full
  name, so every run of `mo test` tries the same inputs. Sizes grow as the run
  goes on. The generator hashes with its own fixed key, not the VM's, which
  changes from run to run (`bytecode.md` section 8).
- **Hidden types.** A type exposed without its insides is built only through
  calls to its module's exposed functions that return it, in sequences of up
  to four calls from generated arguments. A builder that returns
  `Result(T, E)` or `Option(T)` counts when it succeeds.
- **Functions** are generated as constant functions, the identity where the
  types allow it, and functions that derive their result from their
  argument's structural hash, so equal arguments give equal results. In
  `mo test`, the compiler emits these three as `closure` functions for every
  function type a promise's parameters use, and the generator makes closures
  of them.
- **Capabilities** are fresh fakes with generated contents.
- **The budget** is 200 calls of the function per promise, boundaries
  included, and 2 seconds; both can be changed with `mo test --calls` and
  `--seconds`.
- **When the body is false**, the input is shrunk: numbers toward 0, texts and
  collections toward fewer elements, records field by field, for at most 200
  more calls. The report shows the smallest failing input, the result, and
  which line of the body was false. A call that stops is reported as a bug,
  with its input (reference 12.2).

### 10.2 JSON output

`mo check --json` and `mo test --json` write JSON Lines to standard output:
one object per line, written as soon as it is complete, so a run cut short
leaves what it found. Each object's `record` field says what it is. Nothing
else goes to standard output: anything else, such as progress, goes to
standard error. A mistake in the command writes only its message, to standard
error, and exits with status 2. The format keeps reference 13.4's promise: within a major version, fields
keep their names, types and meanings, and codes their meanings; fields and
records may be added, and readers ignore what they don't know.

- **`version`**, always first: `format`, the format's version, `"1.0"`; `mo`,
  Mo's version; and `command`, `"check"` or `"test"`.
- **`diagnostic`**, one per error, sorted by position (3.5): `code`, such as
  `"E0412"`; `headline`; `reason`, and `section`, such as `"8.3"`, both
  `null` for the codes of limits, which cite no section; `primary`, a labeled
  span; `secondary`, a list of them; and `fixes`, the alternatives, each a
  `title` and its `edits`, applied together, each edit a `span` and the `text`
  that replaces it.
- **`test`**, one per example, promise and test, written by `--list`: `name`,
  `kind` and `span`, as in `result`.
- **`result`**, one per example, promise and test run: `name`, its full name
  (`bytecode.md` section 3.5); `kind`, `"example"`, `"promise"` or `"test"`;
  `span`; `outcome`, `"passed"`, `"failed"` or `"stopped"`; and
  `microseconds`, how long it took. A failed example, or a failed test whose
  `assert` compares with `==`, adds `left` and `right`, its two sides' values;
  a failed test adds `line`, the span of the `assert`. A failed promise adds
  `input`, the smallest failing input, as each parameter's `name` and
  `value`; `result`, the call's result; `line`, the span of the line of the
  body that was false; and `calls`, how many calls were tried. A stopped one
  adds `defect`, the defect report of section 8: `reason`, `span`, `stack`,
  each frame's `function` and `span`, and `variables`, each a `name` and
  `value`; and for a promise, the `input` it stopped on.
- **`totals`**, always last: `errors`, the diagnostics written, and
  `dropped`, those found past the limits a run keeps
  (`MO_LIMIT_DIAGNOSTICS`); for `mo test`, `passed`, `failed` and `stopped`;
  and `status`, the status `mo` exits with.

A **span** is `file`, the path of the file; `start` and `end`, byte offsets
into it, `end` excluded; and `line` and `column` where it starts, and
`end_line` and `end_column` where it ends, counted from 1, a column counting
characters as messages do (`src/base/diag.h`). A labeled span adds `label`. A
**value** is a string holding its structural text form (`bytecode.md` section
8), since JSON's numbers can't hold every `Int`, `Decimal` or `Time` exactly.

If `count` in `examples/wc.mo` miscounted words, `mo test --json
examples/wc.mo` would write these lines, with the four results that pass
among them:

```json
{"record":"version","format":"1.0","mo":"0.1.0","command":"test"}
{"record":"result","name":"Wc.count.example.3","kind":"example","span":{"file":"examples/wc.mo","start":453,"end":513,"line":16,"column":5,"end_line":16,"end_column":65},"outcome":"failed","microseconds":41,"left":"Counts(lines: 0, words: 1, bytes: 10)","right":"Counts(lines: 0, words: 2, bytes: 10)"}
{"record":"totals","errors":0,"dropped":0,"passed":4,"failed":1,"stopped":0,"status":1}
```

## 11. Testing strategy

| Layer | How it is tested |
| --- | --- |
| Every C module | unit tests in `tests/`, run plain and under AddressSanitizer and UndefinedBehaviorSanitizer (condition 1) |
| Reference counting | a debug heap under `make sanitize` that records every allocation, poisons freed memory, and checks at each process's end that every object it didn't make immortal was freed |
| Parser | the executable grammar's tests, and a fuzz target that checks the parser and `grammar/mo.y` accept and reject the same inputs |
| Checker | a corpus of programs with the exact diagnostics they must produce, one file or more per error code |
| Lowering | the twelve samples and the corpus, lowered and compared with their Core text, which the Lean evaluator runs to the samples' expected results |
| Core and the VM | **differential testing**: Core programs, hand-written, from the samples, and generated well-typed, run on the Lean evaluator and through the C compiler and VM; any difference in outcome is a bug, and a minimized corpus of them runs in CI, as in AWS Cedar (research brief on verification-guided development) |
| Compiler | every Core program the Core checker accepts compiles to bytecode the verifier accepts: a property run on every Core program above |
| Verifier and loader | arbitrary and mutated files; disassembling then assembling gives the same bytes |
| Library | `mo test` on the library's own examples, promises and tests; the trusted primitives against their oracles (4.5) |
| Small critical C functions | CBMC proofs of memory safety and of no overflow: checked arithmetic, Decimal's products and quotients, UTF-8 decoding, path resolution, hash-table probing, reference count updates |
| The whole toolchain | a golden corpus of programs with their expected output and exit status, and Phase 2's agent benchmark |

**Fuzz targets** (condition 2), each with its invariant:

- the parser against the grammar: same verdict;
- the checker on mutated programs: diagnostics only, never a crash or a failed
  assertion;
- the Core checker on mutated Core text: the same;
- the verifier and loader on arbitrary and mutated bytecode: refuse, or load
  and run without a sanitizer error;
- the VM on generated valid bytecode, with a step budget: an outcome, never a
  crash;
- the text parsers `to_int`, `to_decimal`, `to_float`, `Time.parse` and
  `decode_utf8`: agree with their oracles;
- path resolution in the host: never resolves outside the folder;
- the formatter: formatting twice gives the same text, and keeps the tree.

Every check runs through `ci/check.sh`, in the CI orb on request (D214). The
Lean build (`make formal`) and CBMC need elan and CBMC in `.agents/setup`.

## 12. Phase 2's streams and their interfaces

Phase 2 starts with the contracts as code, then splits into four streams that
build in parallel.

| Stream | Builds | Consumes | Produces | Tests itself with |
| --- | --- | --- | --- | --- |
| 0. Contracts (first) | `bytecode.h`, `types.h`, `heap.h`, `prims.def`, `hostops.def`, `core.h`; Core's text form in C and Lean; in `formal/`, what 4.1 lists as missing, and `Random` as section 7 defines it | the design documents | the four contracts | round trips of the text forms; the Lean build |
| 1. Front end | `syntax`, `check`, `lower` | source files | Core, or diagnostics | the grammar, the diagnostics corpus, and Core text run by the Lean evaluator |
| 2. Compiler | `core`'s checker, `codegen` | Core | bytecode, through `src/bytecode`'s writer | the verifier, and the property that checked Core compiles to verified bytecode |
| 3. VM | all of `src/bytecode`: writer, reader, verifier, assembler and disassembler; `heap`, `prim`, `host`, `vm` | bytecode | outcomes | hand-written bytecode in the text form, with golden outcomes; then the differential tests |
| 4. Library and tools | `lib/`, `src/tools` | the other three | the `mo` command | `mo test` on `lib/`, and the golden corpus |

The headers each stream codes against, in outline. Every pointer a function
returns belongs to the arena it was given, unless its comment says otherwise.

The complete contracts in the headers win over this outline. Types and layouts
have per-kind wire encodings in `bytecode.md` section 3, never native C struct
bytes. Source signatures have at most 255 parameters (E1009); bytecode has
256 slots so a promise can add its result and closure code can add `env`.
Stream 3 implements the writer, text tools, image accessor and C value APIs
declared below. The image retains file indices in every decoded and runtime
table, so streams 2 and 4 use the same indices in calls and value builders.

```c
/* src/base/diag.h: diagnostics, rendered as text or JSON by src/tools. */
struct mo_span { uint32_t file; uint32_t start; uint32_t end; };     /* byte offsets */
struct mo_label { struct mo_span span; const char *text; };
struct mo_edit { struct mo_span span; const char *replacement; };   /* an empty span inserts */
/* Edits apply together or not at all, each against the original text, and don't overlap. */
struct mo_fix { const char *title; const struct mo_edit *edits; uint32_t edit_count; };
struct mo_diag {
    uint16_t code;                      /* E0412, registered in src/base/codes.h */
    struct mo_label primary;
    const struct mo_label *secondary;   /* secondary_count of them */
    uint32_t secondary_count;
    const char *message;                /* the headline, in the reference's words */
    const char *reason;                 /* why, citing the section; NULL only for a limit's code */
    const char *section;                /* the reference section it cites, such as "8.3" */
    const struct mo_fix *fixes;         /* fix_count alternatives */
    uint32_t fix_count;
};
/* Diagnostics own their strings, in their own arena. */
MO_MUST_USE bool mo_diags_init(struct mo_diags *diags);
void mo_diags_free(struct mo_diags *diags);
/* Source files, indexed by mo_span.file, for printing snippets. A file that can't be added
 * adds nothing, and error_out says why: unreadable, past a limit, or no memory. */
MO_MUST_USE bool mo_sources_init(struct mo_sources *sources);
void mo_sources_free(struct mo_sources *sources);
MO_MUST_USE bool mo_sources_add(struct mo_sources *sources, const char *path, uint32_t *file_out,
                                struct mo_source_error *error_out);

/* src/syntax/syntax.h: on an error, a partial tree for the rest of the file, and diagnostics. */
MO_MUST_USE struct mo_module_syntax *mo_parse(struct mo_arena *arena, const struct mo_sources *sources,
                                              uint32_t file, struct mo_diags *diags);
MO_MUST_USE bool mo_read_header(struct mo_arena *arena, const struct mo_sources *sources, uint32_t file,
                                struct mo_header *header_out, struct mo_diags *diags);

/* src/check/check.h: the whole program, checked, or NULL with diagnostics. */
MO_MUST_USE struct mo_program *mo_check(struct mo_arena *arena, struct mo_module_syntax *const *modules,
                                        uint32_t count, struct mo_diags *diags);

/* src/lower/lower.h: lowering and monomorphization. */
MO_MUST_USE struct mo_core *mo_lower(struct mo_arena *arena, const struct mo_program *program,
                                     struct mo_diags *diags);

/* src/core/core.h: Core's checker and its text form. */
MO_MUST_USE bool mo_core_check(const struct mo_core *core, struct mo_diags *diags);
MO_MUST_USE bool mo_core_print(const struct mo_core *core, struct mo_buffer *out);
MO_MUST_USE struct mo_core *mo_core_parse(struct mo_arena *arena, const char *text, size_t length,
                                          struct mo_diags *diags);

/* src/codegen/codegen.h: the whole program, spec code included. */
MO_MUST_USE bool mo_compile(const struct mo_core *core, struct mo_buffer *bytecode_out, struct mo_diags *diags);

/* src/bytecode/bytecode.h: writing and text tools borrow inputs. out is an initialized,
 * empty caller-owned buffer, never input storage. On false it is empty (capacity may have
 * grown), and required error_out gives static reason text. The writer verifies, preserves
 * indices and regenerates the header/checksum. Names must already be in the strings table. */
struct mo_write_error { const char *reason; };
struct mo_assemble_error { uint32_t offset; const char *reason; };  /* text byte offset */
struct mo_load_error { uint32_t offset; const char *reason; };       /* reason is static text */
MO_MUST_USE bool mo_bytecode_write(const struct mo_bytecode *file, struct mo_buffer *out,
                                   struct mo_write_error *error_out);
MO_MUST_USE bool mo_bytecode_assemble(const char *text, size_t length, struct mo_buffer *out,
                                      struct mo_assemble_error *error_out);
MO_MUST_USE bool mo_bytecode_disassemble(const uint8_t *bytes, size_t length, struct mo_buffer *out,
                                         struct mo_load_error *error_out);
/* Loading always verifies and copies what it needs from bytes. The accessor's read-only
 * tables live until mo_image_free, retaining file indices and module contents_exposed. */
MO_MUST_USE struct mo_image *mo_load(const uint8_t *bytes, size_t length, struct mo_load_error *error_out);
MO_MUST_USE const struct mo_bytecode *mo_image_bytecode(const struct mo_image *image);
void mo_image_free(struct mo_image *image);

/* src/vm/vm.h: initialization, processes and calls. */
enum mo_outcome { MO_RETURNED, MO_STOPPED, MO_FAILED };
/* Runs the init functions once; on a stop, the image can't run and defect_out says why. */
MO_MUST_USE enum mo_outcome mo_image_init(struct mo_image *image, const struct mo_limits *limits,
                                          struct mo_defect *defect_out);
/* A process must not outlive its image, which must be initialized. */
MO_MUST_USE struct mo_process *mo_process_new(const struct mo_image *image, const struct mo_limits *limits);
void mo_process_free(struct mo_process *process);    /* frees its whole heap, never the image's */
/* Arguments passed to owned parameters are consumed on every outcome; those passed to borrowed
 * ones stay the caller's. Duplicate before the call to retain an owned argument for a promise.
 * Only MO_RETURNED writes an owned result_out, which C may drop early. On stop/failure it is
 * unchanged, and defect_out describes why, valid until mo_process_free. */
MO_MUST_USE enum mo_outcome mo_call(struct mo_process *process, uint32_t function,
                                    const struct mo_cell *arguments, uint32_t argument_count,
                                    struct mo_cell *result_out, struct mo_defect *defect_out);
MO_MUST_USE bool mo_render_defect(const struct mo_process *process, const struct mo_defect *defect,
                                  struct mo_buffer *out);
/* Initializes the image, then runs main with real capabilities; returns the exit status. */
MO_MUST_USE int mo_run(struct mo_image *image, const struct mo_run_options *options);

/* src/heap/heap.h: C ownership is type-directed. Both operations are infallible, copy/do nothing
 * for immediates, and follow bytecode 2.4's immortal and saturating counts. dup returns another
 * owned cell without consuming its input; drop consumes one owned cell without C recursion. */
MO_MUST_USE struct mo_cell mo_value_dup(struct mo_process *process, uint32_t type, struct mo_cell value);
void mo_value_drop(struct mo_process *process, uint32_t type, struct mo_cell value);
/* Building generated inputs borrows input cells and duplicates references kept. Successful
 * results are owned in the process heap; false leaves inputs and out unchanged. Byte builders
 * copy at most MO_LIMIT_STRING_SIZE bytes; NULL is allowed only for zero length. Bytes accepts
 * any sequence. BigInt uses base.h's sign enum and bytecode 3.1's canonical magnitude; invalid
 * signs/magnitudes, lengths past the limit and allocation/heap-limit failure return false. */
MO_MUST_USE bool mo_value_text(struct mo_process *process, const char *utf8, size_t length, struct mo_cell *out);
MO_MUST_USE bool mo_value_bytes(struct mo_process *process, const uint8_t *bytes, size_t length, struct mo_cell *out);
MO_MUST_USE bool mo_value_bigint(struct mo_process *process, enum mo_bigint_sign sign,
                                 const uint8_t *magnitude, size_t length, struct mo_cell *out);
MO_MUST_USE bool mo_value_construct(struct mo_process *process, uint32_t layout, const struct mo_cell *fields,
                                    uint32_t count, struct mo_cell *out);
MO_MUST_USE bool mo_value_case(struct mo_process *process, uint32_t type, uint32_t tag, const struct mo_cell *fields,
                               uint32_t count, struct mo_cell *out);
MO_MUST_USE bool mo_value_list(struct mo_process *process, uint32_t type, const struct mo_cell *elements,
                               uint32_t count, struct mo_cell *out);
MO_MUST_USE bool mo_value_map(struct mo_process *process, uint32_t type, const struct mo_cell *keys,
                              const struct mo_cell *values, uint32_t count, struct mo_cell *out);
MO_MUST_USE bool mo_value_set(struct mo_process *process, uint32_t type, const struct mo_cell *elements,
                              uint32_t count, struct mo_cell *out);
MO_MUST_USE bool mo_value_closure(struct mo_process *process, uint32_t function, const struct mo_cell *captures,
                                  uint32_t count, struct mo_cell *out);

/* src/host/host.h: a fake for mo test's generated inputs, as its constructor would make it
 * from contents, such as Files.in_memory's map. */
MO_MUST_USE bool mo_host_fake(struct mo_process *process, enum mo_capability which, const struct mo_cell *contents,
                              uint32_t count, struct mo_cell *out);
```

The order of work: stream 0 first, for about a week, reviewed against these
documents. Then streams 1, 2 and 3 in parallel, each tested on its own as
the table says, and stream 4 throughout. The differential tests join streams
1 to 3 as soon as each can run its part.

**Repository changes for Phase 2.** The Makefile builds `src/<module>/*.c` per
module; `grammar/` includes `src/base/base.h` by its new path; `.agents/setup`
adds elan, with Lean 4.34.0, and CBMC; `ci/check.sh` gains `make formal` and
the CBMC proofs, within a time budget for the CI orb.

## 13. Open questions

### 13.1 For engineering, measured in Phase 2

- **Monomorphization's cost.** Instantiating every generic function at every
  type it is used with keeps the VM simple and fast, but grows code. If it
  grows too much, generic code can share one instantiation for all
  reference types, whose cells look the same.
- **Packed lists.** Every element is a 16-byte cell, so a `List(Int)` uses
  twice the memory it needs. Packing lists of small immediates is decided
  with the benchmark.
- **Text's character count** is computed on each call. Caching it costs a word
  per text; decide with the benchmark.
- **Unicode16** needs tables generated from the Unicode Character Database
  16.0, kept in the repository with the generator and the source files'
  hashes.
- **The bytecode format's stability.** It is internal in Phase 2. Phase 5's
  upgrades need a versioned format; decide then whether it is public.

### 13.2 Settled with Robert

Writing this architecture and Mo Core found places where the reference didn't
say what a program means. Robert decided all of them on 12 October 2026 (D227),
and the reference now says: when an anonymous function may read a `var`
(5.6); which built-in types have which abilities (9.2); where `printed` can be
used (12.4); repeated keys in map literals (3.8); paths that leave a folder,
line endings, invalid UTF-8 on standard input, and times as text (14); the
order of constants (10.4); polymorphic recursion (5.5); the limits of a few
library functions (14); and the `mo` command's exit status (13.4). The same
day he settled the `mo` command for agents (D233): JSON output with a written
promise, choosing tests by exact name or module, and limits as flags, with
`src/base/limits.h`'s defaults (reference 13.4; sections 10 and 10.2).
