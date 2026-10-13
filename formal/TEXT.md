# Core text, version 0.7

The Lean implementation in `MoCore/Text/` is the reference codec. Every file
is `(core 0.7 PAYLOAD)` followed by LF. Constructors are case-sensitive and
named after Lean. A nullary constructor is still a list: `(unit)`.

This is an interchange format, not Mo source or the informal notation in
TRANSLATION.md. There are two result representations: lossless constructor
transport (`Outcome`, including closures), and observable reports (`Report`,
the protocol `mocore` writes). The latter uses reference 9.2's structural
value spelling inside a string. C differential tests compare **reports**.

## Changes from 0.6

Version 0.7 implements D237. `(io (makeFolder))` takes Files, Text and returns
Result(Unit, FileError); there is no ReadFiles or spec-only form. World and
CapState gain a mandatory folder-string list after RandomState, before
World's fake store. Reports gain real folders after files, before fake states.
Folders persist after removing their last file. Writes/appends require an
existing parent; only makeFolder creates missing folders. Folder discovery,
ordering, confinement and failure precedence are specified below and in
TRANSLATION.md's D237 decisions. No implementation error code is added.
Only 0.7 is accepted; 0.6, older and unknown versions are rejected.

### Earlier changes from 0.5

Version 0.6 implements D236: `Bug` gains `(fileFolderConflict S)` naming a
file/folder collision refused by `Files.in_memory` before allocation. File
kind mismatches use `Failed` with the exact reasons `"not a folder"` and
`"is a folder"` for real modeled handles and fakes. Float.to_text keeps the
shortest round-trip digits but uses fixed notation from 1e-6 through below
1e21; both zeros stay plain `0`/`-0`. Float wire values and structural
reports still use bits. Symbolic-link resolution remains host behavior,
to be verified on real folders by VM/host tests, not by this text-file model.
In 0.6 no existing record shape, primitive signature or metadata field changed.
Version 0.7 supersedes its implicit-folder creation and lifetime rules.

### Earlier changes from 0.4

Version 0.5 added a mandatory compiler-metadata field to Program. Expressions,
typing, evaluation and proofs were unchanged. Metadata is preserved
losslessly, checked for shape at program decoding, and never read by `mocore`
execution or structural observations. A shape error is `parseError`, not
`illTyped`. There are no omitted/default fields on the wire.

### Earlier changes from 0.3

Version 0.4 adds BigOp `min`, `max`, `pow`, `floorDiv`, `floorMod`, FloatOp
`rem`, `toDecimal`, and TextOp `toFloat`. Decimal-to-Float and decimal text
parsing round an exact rational once to binary64, including ties, subnormals
and overflow; Float-to-Decimal rounds its shortest decimal to 18 places.
Numeric signatures and choices are recorded below and in TRANSLATION.md.
Report tags and record layouts other than Program's metadata are unchanged.

### Earlier changes from 0.2

Version 0.3 adds types `(entry)` and `(entryKind)`, `IoOp.entries` with its
read-only Boolean, and the five `Prim.unicode16` operations. The eighth
field of World/CapState changes from a natural seed to a mandatory
`RandomState` record of four explicit 64-bit words. Seeded and real Random
now use xoshiro256**/SplitMix64/Lemire, not the placeholder reduction.
Reached Unicode16 calls stop at the trusted-oracle boundary described below.
Report tags, exit codes, real/fake handle provenance and the fake-state
store added in 0.2 are unchanged. There are no
omitted/default fields on the wire or silent conversions of old seeds.

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
be nonnegative. Float bits and random words are `0x` and exactly 16 hex digits; input allows
either case, printing uses lowercase and preserves every bit, including NaN
payloads and negative zero. Names are always strings, never symbols.

Parsing is not type checking: an out-of-range integer/byte literal can be
parsed, then refused by the checker.

C limits each input and each appended canonical document to 8 MiB
(`MO_LIMIT_CORE_TEXT_SIZE`), including envelope, escaped strings and final
LF. Printing is append-and-rollback: failure restores the earlier buffer
length. Earlier contents do not count toward this document's limit; the
buffer's own limit is an independent constraint. Lean's codec does not
impose the C resource limits.

```ebnf
file = "(", "core", "0.7", payload, ")" ;
sexp = atom | string | "(", { sexp }, ")" ;
integer = [ "-" ], digit, { digit } ;
bits = "0x", hex-digit (* exactly sixteen *) ;
type = "(int)" | "(bigInt)" | "(byte)" | "(decimal)" | "(float)"
     | "(bool)" | "(char)" | "(text)" | "(bytes)" | "(unit)"
     | "(range)" | "(time)" | "(duration)" | "(ordering)" | "(fileError)"
     | "(entry)" | "(entryKind)"
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
(core 0.7 (map (text) (list (byte))))
(core 0.7 (float 0x8000000000000000))
(core 0.7 (text "hello\nβ"))
```

Files are UTF-8 without a BOM; invalid UTF-8 is a parse error. An atom is a
nonempty run of scalars other than the delimiters defined above. Symbols
are atoms used as constructor names, `core`, `0.7`, `true` and `false`;
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
Program   := (Program (DataDecl*) (FunDecl*) (ConstDecl*) S Metadata)
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
field/arm lists are still present. Source locations live in the side tables
below, never in expression constructors.

### Compiler metadata is beside the meaning

The final Program field is mandatory. Its indices refer to the declaration
lists above, not future bytecode indices. `U` below is a natural bounded by
2³²−1; `opt X` is `(none)` or `(some X)`. C uses `uint32_t`, and spans are
`struct mo_span`. Lean uses Nat and validates these bounds/references.

```text
Metadata    := (noSource) | (Metadata (S*) (ModuleMeta*) (FunMeta*) (ConstMeta*))
ModuleMeta  := (ModuleMeta S (U*) (U*) (U*) ((U B)*) (U*))
FunMeta     := (FunMeta S (opt SourceSpan) (opt S) BodyMeta (opt U) (opt B) (SpecLine*))
ConstMeta   := (ConstMeta S (opt SourceSpan) BodyMeta)
BodyMeta    := (BodyMeta ((opt SourceSpan)*) ((opt S)*) (BindingMeta*) ((U S)*))
BindingMeta := (BindingMeta U U S)
SourceSpan  := (SourceSpan U U U)
SpecLine    := (SpecLine (opt SourceSpan) S)
```

- **Files:** every `mo_sources.files[i].path`, copied in exactly that order,
  including unused files; `SourceSpan.file = i`. Spans are half-open UTF-8
  byte offsets, `start ≤ end`. The transport checks file bounds and u32
  range, not source contents/lengths. Lowering must check offsets against the
  original source and copy text now; compilation never opens source files.
  Filenames, module names, display names and source binding names are nonempty
  and NUL-free. Spec text may be empty and may contain NUL/escaped controls.
- **Modules:** name; owned data, function and constant indices; exposed data
  indices paired with `contents_exposed`; exposed function indices. These
  ownership lists partition each declaration list exactly once. Function
  ownership agrees with `FunDecl.module`. Exports are distinct members of
  that module, and only ordinary functions are exposed. A type absent from
  exports is private, `(i false)` is `T`, and `(i true)` is `T(..)`. All types
  retain complete constructors/fields for VM layouts and structural printing;
  exposure restricts tools, not evaluation. Modules are unique, in loader
  order; member and export lists are in source declaration order. Generic
  function instances belong to the requesting module (architecture 4.2);
  generic data declarations belong to their defining module. Constant ownership
  order is independent of Program's dependency order. Files and modules have
  at most 4096 entries, matching C's limits.
- **Functions/constants:** metadata lists align exactly with Program's
  declaration lists. `name` is the full bytecode/display name of bytecode 3.5,
  independent of the Core lookup key; a constant's init name is `name + ".init"`.
  Lowering makes display names globally unambiguous, assigns spec and closure
  numbers in source order *before* rewriting the bodies, and retains overload
  and monomorphic-instance identity. Declaration spans support listing/results.
  `text` is absent on ordinary functions, present on all four spec kinds: a
  test's decoded sentence; the verbatim example/anchor line; a promise's
  header line. Verbatim lines exclude indentation and CR/LF, retain internal
  spacing/comments, and may contain line breaks inside a literal via escapes.
- **Spec links:** a promise's `subject` is its ordinary function's index; an
  anchor's is its promise's index. Others have no subject. Only anchors have
  `allows`: true requires 0, false requires nonzero. A promise has one
  `SpecLine` per condition, in source order (at least one); other kinds have
  none. Its Int result n > 0 selects line n − 1. Lowering supplies matching
  subject parameters/result and accurate condition spans/text; shape checks
  validate kinds and bounds, not truth or a correspondence to generated code.
- **Body traversal:** start at the root with index 0. Visit each Expr before
  its children, in the expression schema's field order, and lists/arms in
  their existing order. Count only Expr constructors, not types, literals,
  parameters or arms. Traverse lambda bodies too. Thus `join` visits body
  before rest, and `case_` visits its scrutinee before arm bodies. Every body,
  including a constant initializer, has **exactly one optional span per node**.
  Synthesized nodes use the nearest responsible source construct's span;
  `none` means genuinely source-less, never file 0/offset 0 by convention.
- **Bindings:** `BodyMeta.params` has one optional source name per top-level
  function parameter (empty for constants). `none` denotes a generated
  parameter. Each BindingMeta is node index, binder slot, original name.
  At `let_`, slot 0 is its binder; at `lam`/`join`, slots follow parameter
  order; at `case_`, flatten arm bound names in arm order. No other node binds.
  Records are sparse, strictly ordered by (node, slot); an absent slot is
  generated, not a source variable. Multiple occurrences (SSA versions,
  loop parameters, pattern lowering) may carry the same source name; compiler
  register live ranges distinguish them. Core lexical lookup resolves uses
  to these binding occurrences, including shadowing and closure captures.
  `BodyMeta.closures` has exactly one (node, full name) pair per `lam`, in
  preorder. Nested names/locations survive lifting without another source pass.

Lowering must fill **all** source metadata, including library instances,
entry wrappers, constants and spec code, and update the tables whenever it
rewrites a body. Compiler passes carry provenance through lifting, register
allocation and optimization; they compute instruction offsets and readable
register ranges themselves. Generated instructions use their responsible
node/declaration span. Shape validation (`metadataValid` in Lean,
`mo_core_metadata_valid` in C) is separate from `checkProgram`/Core typing.
C parsing and printing reject malformed shapes; Lean printing is lossless
even for malformed in-memory metadata, while decoding rejects it. Changing
valid metadata can change bytecode/debugging/tools, **never a Core outcome**.

**Handwritten Lean programs use `(noSource)`.** This is a deliberate whole
document mode, not an empty source envelope or permission for `mo_lower` to
omit metadata. It compiles with empty spans/register names/spec text and no
exports/spec links/condition lines. Functions retain their resolved names
and modules; all nominal data/constants belong to a synthetic `%Core` module
(first), followed by function modules in first-occurrence order. Constants
retain lookup names, with `.init` appended; closures use the enclosing lookup
name plus `.fn.N`, numbered in preorder. Promise generation/anchor checking
requiring surface links is unavailable in this mode; direct Core/VM runs and
selected tests/examples remain usable. No filenames, exposure or wording are
invented. `Metadata.core` is an explicitly synthetic full-envelope fixture,
with two files/modules, both exposure modes, all spec kinds, closures and
bindings, and an absent span. The other Lean-written fixtures use `noSource`.

## Every primitive

```text
P := (int IntOp) | (big BigOp) | (byte ByteOp) | (dec DecOp)
   | (float FloatOp) | (not) | (cmp Base CmpOp) | (char CharOp)
   | (text TextOp) | (bytes BytesOp) | (range RangeOp) | (time TimeOp)
   | (unicode16 UnicodeOp)
   | (list T ListOp) | (map T T MapOp) | (set T SetOp) | (eq T) | (io IoOp)
```

Each name in the following table means a **nullary S-expression**, e.g.
`(checkedAdd)`, not an atom. The lists are exhaustive; casing matters.

| Family | Nullary constructors |
| --- | --- |
| `IntOp` | `add sub mul div rem neg abs min max pow floorDiv floorMod checkedAdd checkedSub checkedMul wrappingAdd wrappingSub wrappingMul saturatingAdd saturatingSub saturatingMul toText toDecimal toBig toFloat toChar toByte seconds milliseconds minutes hours days` |
| `BigOp` | `add sub mul div rem neg abs min max pow floorDiv floorMod toInt toText` |
| `ByteOp` | `toInt toText` |
| `DecOp` | `add sub mul div rem neg abs min max floor ceil round toInt toText toFixed toFloat` |
| `FloatOp` | `add sub mul div rem neg abs sqrt floor ceil round isNan isInfinite toDecimal toText` |
| `Base` | `int bigInt byte decimal float bool char text bytes time duration` |
| `CmpOp` | `lt le gt ge` |
| `CharOp` | `code isDigit isLetter isWhiteSpace lower upper toText` |
| `TextOp` | `concat byteCount charCount isEmpty chars fromChars lines words split trim trimStart trimEnd startsWith endsWith contains dropPrefix dropSuffix replace repeat padLeft padRight lower upper toInt toDecimal toFloat join bytes` |
| `BytesOp` | `size get decodeUtf8` |
| `UnicodeOp` | `graphemes displayWidth normalizeNfc lowercase uppercase` |
| `RangeOp` | `make start end toList contains toText` |
| `TimeOp` | `sub add subDur checkedAdd toText parse durAdd durSub durMul durNeg inMilliseconds inSeconds durToText` |
| `ListOp` | `size isEmpty get first last append prepend concat reverse take drop set removeAt uncons enumerate contains unique` |
| `MapOp` | `empty size isEmpty get has set remove keys values entries ofList` |
| `SetOp` | `empty size isEmpty has add remove union intersection difference toList ofList` |
| `IoOp` | `argsList envGet print printError readLine printed printedErrors writeText appendText remove makeFolder readOnly now randomInt consoleFake consoleFakeInput filesInMemory clockFixed randomSeeded argsOf envOf` |

The non-nullary operation constructors are exactly:

```text
ListOp additionally := (lit N) | (zip T)
MapOp  additionally := (lit N)
IoOp   additionally := (readText B) | (readBytes B) | (list B) | (entries B) | (exists B) | (within B)
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

### Numeric operations added in 0.4

| Operation | Signature | Meaning |
| --- | --- | --- |
| `(big (min))`, `(big (max))` | BigInt, BigInt → BigInt | smaller/larger operand, first on equality |
| `(big (pow))` | BigInt, Int → BigInt | exact integral power; negative exponent stops; 0⁰ = 1 |
| `(big (floorDiv))`, `(big (floorMod))` | BigInt, BigInt → BigInt | quotient rounds toward −∞; remainder a − floorDiv(a,b) × b; zero divisor stops |
| `(float (rem))` | Float, Float → Float | truncating remainder with dividend sign, including zero; NaN/infinite dividend/zero divisor gives canonical NaN bits `0x7ff8000000000000`; finite dividend with infinite divisor unchanged |
| `(float (toDecimal))` | Float → Option(Decimal) | shortest round-tripping decimal rounded to 18 places, ties to even; nonfinite or out of range gives None |
| `(text (toFloat))` | Text → Option(Float) | reference 14 grammar; nearest binary64, ties to even once; signed underflow zero; numeric overflow gives None; exact `nan`, `inf`, `-inf` accepted |

`(dec (toFloat))` uses the same exact converter on its count divided by
10¹⁸. In particular count 1000000000000000065 returns bits
`0x3ff0000000000000`, not the next float. The shortest decimal coefficient
is the nearest among those with the fewest significant digits that round
back to the original bits, ties to an even coefficient. `%float.toText`
renders fixed notation for shortest decimal exponents -6 through 20,
scientific otherwise: `1000`, `0.000001`, `1e21`, `1.5e-7`, `1e-7`.
Whole floats have no `.0`; zero and negative zero always print `0`/`-0`.
Scientific `e` is lowercase, minus only for negative exponents, without
padding or a plus. `nan`, `inf`, `-inf` are unchanged.
`%float.toDecimal` uses that same decimal's exact
rational value, not the float's exact binary rational (so 0.1 gives 0.1).
These algorithms are executable definitions with type-safety proofs and
regression checks, not proofs of numerical functional correctness.

### Unicode16 and folder entries

`(unicode16 (graphemes))` has signature Text → List(Text),
`(unicode16 (displayWidth))` Text → Int; `(normalizeNfc)`, `(lowercase)`
and `(uppercase)` under `unicode16` each have Text → Text. They are pure
ordinary operations, not spec-only calls or the ASCII Text operations.
Core specifies types and safety, not Unicode tables. After evaluating
arguments, a **reached** call stops with the exact report below (exit 1),
where effects are everything produced so far:

```text
(core 0.7 (stopped (outside "Unicode16 is a trusted primitive unavailable in mocore") (effects () () () () ())))
```

An unchosen branch or unused function containing such a call still runs.
Do not compare this boundary stop to VM behavior: runtime Unicode16 must
instead be checked against the Unicode Character Database 16.0's own test
files (architecture 4). No fallback Unicode results are invented here.

`(io (entries false))` takes Files, Text; `true` takes ReadFiles, Text.
Both return Result(List(Entry), FileError), using `list`'s paths, errors
and Unicode-scalar sorted order. `Entry` has constructor `Entry` with
fields `name: Text`, `kind: EntryKind`; the kind constructors, in case-arm
order, are `File`, `Folder`, `Link`, `Other`, all nullary. No DataDecl is
needed. A construction and its lossless value are:

```text
(con (entry) "Entry" ((lit (text "notes")) (con (entryKind) "Folder" ())))
(con "Entry" ((text "notes") (con "Folder" ())))
```

The text-file maps and explicit folder lists used for both real modeled
handles and in-memory fakes cannot represent links or special files.
Those kind values still type-check and transport; the OS host must classify
them without following links. The top folder always exists; other folders
exist by membership in the folder list, even when empty. `Files.in_memory`
records every file ancestor before allocation and refuses any key that is
also a folder, with `(fileFolderConflict "path")` (exit 1): exact stored
keys and slash prefixes are compared, without normalization. The first
conflicting file key in insertion order is reported, including `""` if a
key tries to replace the implicit root. No fake state is allocated on error.
Arbitrary host World/CapState maps and folder lists still decode losslessly;
the folder list is authoritative, and the codecs do not repair inconsistent
host states. `entries` classifies stored folders as Folder, other names as File.
Names are immediate names, not relative paths;
old lists remain snapshots after a shared/narrowed/read-only handle changes
the map. `walk` is Mo library code in `lib/`, not a Core operation.

Result-returning file operations check kinds after lexical confinement.
`list`/`entries`/`makeFolder` on a file give `Error(Failed(path, "not a folder"))`.
`readText`, `readBytes`, `writeText`, `appendText` and `remove` on a folder
(including `"."`) give `Error(Failed(path, "is a folder"))`. Using a file
as an intermediate folder or a `within` root gives `"not a folder"` for
all Result operations, even reading `"."`. Errors preserve the caller's
original path and leave state unchanged. `exists` is false through a file
root/intermediate component. `within` and `readOnly` still derive handles
without returning Result; invalid lexical roots remain inaccessible.
Missing reads/removes/lists/entries remain `NotFound`; writes/appends create
files only if the parent folder exists, otherwise `NotFound(originalPath)`.
`makeFolder` creates the target and missing ancestors, parent-first, and is
idempotent. Removing a file leaves all folders intact. A missing narrowed
root gives `NotFound` for every Result call (including makeFolder) and false
for exists; a wider handle must create that root first. `within` stays lazy.

| Mo operation | File target | Folder target (including empty) | Missing target |
| --- | --- | --- | --- |
| `read_text` | `Ok(text)` | `Failed(path, "is a folder")` | `NotFound(path)` |
| `read_bytes` | `Ok(UTF-8 bytes)` | `Failed(path, "is a folder")` | `NotFound(path)` |
| `write_text` | replace, `Ok(Unit)` | `Failed(path, "is a folder")` | create if parent exists; otherwise `NotFound(path)` |
| `append_text` | append, `Ok(Unit)` | `Failed(path, "is a folder")` | create if parent exists; otherwise `NotFound(path)` |
| `remove` | delete, `Ok(Unit)`; keep folders | `Failed(path, "is a folder")` | `NotFound(path)` |
| `make_folder` | `Failed(path, "not a folder")` | `Ok(Unit)`, unchanged | create target and ancestors, `Ok(Unit)` |
| `list` | `Failed(path, "not a folder")` | `Ok(sorted names)` | `NotFound(path)` |
| `entries` | `Failed(path, "not a folder")` | `Ok(sorted entries)` | `NotFound(path)` |
| `exists?` | true | true | false |
| `within` | retain root; Result calls fail `"not a folder"`, exists is false | retain root | retain root; Result calls give `NotFound`, exists is false |
| `read_only` | preserve root and shared state, change kind | same | same |
| `Files.in_memory` | allocate unless a key is also a folder | discover and retain file ancestors | empty map makes an empty top folder |

Errors are wrapped in Error, and successful missing-file writes/appends give
Ok(Unit). ReadFiles variants agree; make_folder has no ReadFiles variant.
Lexical refusal wins, then a file root/intermediate component, then a missing
root, then target kind and parent existence. All errors use the caller's
original path and leave state unchanged. TRANSLATION.md records the atomic
creation model and host-only limitations.

Host operations follow relative links that remain inside the capability's
folder; absolute/escaping targets are `NotAllowed`, and more than 40 links
give `Failed(path, "too many links")`. `remove` removes the
final link itself rather than opening its target (intermediate links still
follow confinement). Folder arguments to `list`/`entries` may follow links,
but reported child kinds and `walk` never follow them. These are real-folder
VM/host tests, not representable runs of `mocore`; no links are modeled.

### Fake construction and dispatch

These operation constructors are nullary; their **expression arguments**
have the signatures below. Arguments evaluate strictly, left to right,
before allocation. Only tests, examples, promises, anchors and lambdas
lexically inside spec code may use them, as for `fail`, `printed` and
`printedErrors`. Ordinary callees may use a fake passed by spec code.

| Mo | IoOp | Argument types | Result | Initial state field |
| --- | --- | --- | --- | --- |
| `Console.fake()` | `consoleFake` | none | Console | all defaults |
| `Console.fake_input(lines)` | `consoleFakeInput` | List(Text) | Console | stdin = lines |
| `Files.in_memory(files)` | `filesInMemory` | Map(Text, Text) | Files | files = map entries; folders = their ancestors |
| `Clock.fixed(t)` | `clockFixed` | Time | Clock | now = t |
| `Random.seeded(n)` | `randomSeeded` | Int | Random | random = four SplitMix64 outputs from n modulo 2⁶⁴ |
| `Args.of(list)` | `argsOf` | List(Text) | Args | args = list |
| `Env.of(map)` | `envOf` | Map(Text, Text) | Env | env = map entries |

Default list/map fields are empty, now is zero, and random is seed 0's state
(listed below). Every successful construction
appends one state at index equal to the previous store length, regardless of
kind; the returned root is `(some "")`. Indices are zero-based, never reused
or garbage-collected within a run, and preexisting states count. Two handles
with the same fake index share state; different indices stay separate.
The capability kind selects the relevant fields; `CapState` has no kind tag.
Host-supplied aliases of different kinds therefore address different fields
of the same record. Source constructors cannot create such cross-kind aliases
except Files/ReadFiles, which intentionally share files and folders.

`within` resolves the root as before and preserves the fake index;
`readOnly` changes Files to ReadFiles, preserving root and index. Ordinary
file/console/clock/random/args/env operations read or update only the chosen
state. Real handles use the world's first nine fields. No fake operation
changes those real fields, and no real operation changes a fake state.
`printed`/`printedErrors` return snapshots of that fake console's output/
error list; later printing does not change an earlier result. Input lists and
maps retained by the caller similarly remain unchanged after reads/writes.

A real console observation stops with `outside "console observation needs a fake"`.
A dangling index in unchecked evaluation stops with `outside "invalid fake handle"`;
`mocore` rejects it in host arguments as `invalidRun`, including inside
composites. Lossless codecs still transport such values.

File maps retain their paths and insertion order without constructor-time
normalization. Paths are resolved on use with the existing rules; files hold
UTF-8 texts, and readBytes returns their encoding. Folder existence/listing
rules and D236 kind failures are specified above. Fake input lines are
verbatim texts, including any embedded CR/LF; they are not split or decoded again.

Seeded fakes and real modeled Random use `xoshiro256** 1.0`, seeded
through four consecutive SplitMix64 outputs, starting after its first
increment. Negative seeds are interpreted modulo 2⁶⁴. Raw draws and the
128-bit multiply-high/rejection of Lemire are specified exactly in
TRANSLATION.md's library decisions, with independently compiled reference
vectors in Builtins. `int(low, high)` has exclusive high, supporting spans
1 through 2⁶⁴−1 without signed overflow. Rejected draws advance the state;
an invalid interval stops. The total rejection bound is the full nonzero
state period, 2²⁵⁶−1, not evaluator fuel or a heuristic retry cap.

## Lossless values, worlds and internal outcomes

Lossless values have **all** `Value` constructors. The twelve literal
constructors have exactly the same syntax as literals. Additional forms:

```text
V additionally := (range I I)
   | (tuple (V*)) | (list (V*)) | (map ((V V)*)) | (set (V*))
   | (con S (V*))
   | (closure ((S V)*) ((S T)*) E ((S V)*))
   | (cap capability root fake)
root := (none) | (some S)
fake := (none) | (some N)
RandomState := (RandomState bits bits bits bits)
CapState := (CapState (S*) ((S S)*) (S*) (S*) (S*) ((S S)*) I RandomState (S*))
World := (World (S*) ((S S)*) (S*) (S*) (S*) ((S S)*) I RandomState (S*) (CapState*))
Bug := (overflow S) | (divByZero) | (stop S) | (outside S) | (repeatedKey)
     | (fileFolderConflict S)
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
microseconds since Unix epoch, random state, folders, fake states**. Each `CapState`
has the world's first nine fields in exactly the same order, without a
nested store. Fake state contains only texts, ordered text pairs and numbers,
never values holding handles or closures. Environment and files are
ordered string pairs; duplicate keys are preserved (the model's lookup
finds the first). Files hold UTF-8 texts, not arbitrary bytes. Input lines
are already split: the codec does not remove CR/LF or replace invalid
UTF-8 inside a line. Print calls contribute one list element, even when
that element contains newlines. Initial output/error can be nonempty and
are preserved. Worlds deliberately accept any clock integer and four random
words; they are host state, not checked literals. The words are `s0..s3`
in reference order, never host-endian bytes or a retained seed. The default
state is exactly:

```text
(RandomState 0xe220a8397b1dcdaf 0x6e789e6aa1b965f4 0x06c45d188009454f 0xf88bb8a8724c81ec)
```

Folders are a mandatory ordered string list, not file-map entries or handles.
Source-created state contains each non-root path once: construction discovers
raw file ancestors parent-first in file insertion order, and makeFolder adds
normalized missing prefixes parent-first. Existing folders keep their places;
file removal never changes the list. The root `""` exists implicitly and is
not inserted. For example files `[("z/n", "x"), ("a/b/c", "y")]` initialize
folders `["z", "a", "a/b"]`. Listing still sorts immediate names, independently
of this state order. Codecs retain every string, duplicate, root entry and
order in arbitrary host lists; no folder or ancestor is inferred on decode.
Realistic host state must explicitly supply all existing non-root folders.

There are no wire defaults: this record is required even for states of
non-Random fakes. All-zero words decode losslessly, but a Random.int using
them stops with `outside "Random has an all-zero state"`. An exhausted
period stops with `outside "Random exhausted its period"`; reference
period/coverage correctness, not proved here, excludes this for nonzero
states. Real entropy acquisition is runtime work, never oracle I/O.

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
| Entry / EntryKind | `Entry(name: "notes", kind: Folder)` / `File`, `Folder`, `Link`, `Other` |
| Option / Result / Ordering | positional fields: `None`, `Some(3)`, `Ok(Unit)`, `Error(e)`, `Less`, `Equal`, `Greater` |
| Closure (no Text ability) | `<function>`; apply it to compare its behavior |
| Capability (no Text ability) | its lossless S-expression, e.g. `(cap (readFiles) (some "notes") (some 2))` |

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
(core 0.7 (returned "[\"a\", \"b\\n\"]" (effects () () () () ())))
```

This avoids confusing a top-level Text with a constructor/number, and
allows every Core string name without changing the S-expression lexer.
`reportIn` parses this string as an observation, not back into a `Value`.
Reference spelling alone loses type distinctions (`Int`/`BigInt`/`Byte`),
and `<function>` loses captures; use `valueIn`/`valueOut` for reconstruction.
The full report grammar (not the internal Outcome grammar) is:

```text
effects := (effects (S*) (S*) ((S S)*) (S*) (CapState*))
Report := (returned S effects) | (stopped Bug effects) | (failed S effects)
        | (outOfFuel) | (wrong S) | (parseError S) | (illTyped S)
        | (invalidRun S) | (ioError S)
```

Effects are real standard output, real standard error, real final files, real
final folders and
all final fake states in allocation order, including states with no remaining
handles. Each fake state's full nine fields are compared, including unread
input and all four random words. Real remaining stdin, args, environment,
clock and random state are still not compared. Files include unchanged entries.
A stop or failure retains both real and fake effects up to that point.
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
recursive types, map/set distinctness and that each fake index exists in
the supplied World. Valid fake host arguments are allowed for differential
testing of ordinary entry functions; this does not give ordinary code a fake
constructor. Closures, also nested ones, are
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
(core 0.7
  (Run
    (Program ()
      ((FunDecl "main" () (float) (lit (float 0x8000000000000000))
        "Main" (function) 0))
      () "main" (noSource))
    (World ("argument") (("MODE" "test")) ("input") () () (("a" "hi\n")) 0
      (RandomState 0xe220a8397b1dcdaf 0x6e789e6aa1b965f4 0x06c45d188009454f 0xf88bb8a8724c81ec) () ())
    (entry ()) 30))
```

The exact canonical report (exit 0):

```text
(core 0.7 (returned "0x8000000000000000" (effects () () (("a" "hi\n")) () ())))
```

A named test demonstrating failure without a bug:

```text
(core 0.7
  (Run
    (Program ()
      ((FunDecl "main" () (unit) (lit (unit)) "Main" (function) 0)
       (FunDecl "expected seven" () (unit) (fail (lit (text "got six"))) "Spec" (test) 0))
      () "main" (noSource))
    (World () () () () () () 0
      (RandomState 0xe220a8397b1dcdaf 0x6e789e6aa1b965f4 0x06c45d188009454f 0xf88bb8a8724c81ec) () ())
    (test "expected seven") 30))
```

Its report is `(core 0.7 (failed "got six" (effects () () () () ())))` (exit 2).
Changing fuel to 0 gives `(core 0.7 (outOfFuel))` (exit 3). An ordinary
function with `fail` instead is `illTyped` (exit 5), never evaluated.

A test constructs a console and passes it to ordinary `main`. Observing the
same handle after the call reads its output; real output stays empty:

```text
(core 0.7
  (Run
    (Program ()
      ((FunDecl "main" (("c" (cap (console)))) (unit)
        (prim (io (print)) ((var "c") (lit (text "hello"))))
        "Main" (function) 0)
       (FunDecl "fake" () (unit)
        (let_ "c" (prim (io (consoleFake)) ())
          (let_ "_" (call "main" ((var "c")))
            (if_ (prim (eq (list (text)))
                   ((prim (io (printed)) ((var "c")))
                    (prim (list (text) (lit 1)) ((lit (text "hello"))))))
              (lit (unit)) (fail (lit (text "unexpected output"))))))
        "Spec" (test) 0))
      () "main" (noSource))
    (World () () () () () () 0
      (RandomState 0xe220a8397b1dcdaf 0x6e789e6aa1b965f4 0x06c45d188009454f 0xf88bb8a8724c81ec) () ())
    (test "fake") 50))
```

Its exact report (exit 0) includes the allocated state's full nine fields:

```text
(core 0.7 (returned "Unit" (effects () () () () ((CapState () () () ("hello") () () 0 (RandomState 0xe220a8397b1dcdaf 0x6e789e6aa1b965f4 0x06c45d188009454f 0xf88bb8a8724c81ec) ())))))
```

A D237 run makes two folders, writes their only file, then removes it.
The real folder list retains both empty folders:

```text
(core 0.7
  (Run
    (Program ()
      ((FunDecl "main" (("f" (cap (files)))) (result (unit) (fileError))
        (let_ "_" (prim (io (makeFolder)) ((var "f") (lit (text "a/b"))))
          (let_ "_" (prim (io (writeText))
            ((var "f") (lit (text "a/b/only")) (lit (text "text"))))
            (prim (io (remove)) ((var "f") (lit (text "a/b/only"))))))
        "Main" (function) 0))
      () "main" (noSource))
    (World () () () () () () 0
      (RandomState 0xe220a8397b1dcdaf 0x6e789e6aa1b965f4 0x06c45d188009454f 0xf88bb8a8724c81ec) () ())
    (entry ((cap (files) (some "") (none)))) 50))
```

Its exact report (exit 0):

```text
(core 0.7 (returned "Ok(Unit)" (effects () () () ("a" "a/b") ())))
```

The file protocol regression also calls this ordinary function with a fake
Files handle. After return, stop or failure, its CapState still has folders
`("a" "a/b")`, no files, and no effects on real files/folders. The Builtins
matrix checks both provenances: writes/appends before creation give
`Error(NotFound("new/item"))`; after creation they succeed, append preserves
the earlier text, and after removal all empty folders remain visible to
`list`, `entries`, `exists?` and shared narrowed/read-only handles.

The exporter below writes all twelve real programs with their timing worlds
and argument values, plus `Fakes.core`, a named spec exercising all seven
constructors, independent states, snapshots, shared random handles in closures
and shared file writes through an ordinary function. `Library.core` tests
Lemire results, maximum-span Int intervals, entries and the D236/D237 file
matrix; its five Unicode16
functions are present and checked but not called by that test. `JsonQuery.core`
is the renamed `examples/json-query.mo` translation, module JsonQuery.
`Metadata.core` exercises the full compiler envelope on a synthetic program
returning 12; it does not assert source provenance for any Mo example.
Wc's run returns the **Core value** `1` but the oracle
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
tests actual file input, UTF-8 rejection, exits, retained stop effects and
all five reached Unicode16 operations versus a skipped one.
Regenerate **both** corpora from `formal/`:

```sh
lake env lean --run ExportSamples.lean ../tests/core ../fuzz/corpus/fuzz_core_text
```

`ci/check_core_fixtures.sh` compares all 15 run fixtures and 14 canonical
fuzz seeds with fresh Lean output. C's schema-driven codec round-trips
every fixture and all new types/operations/state records; the fuzzer tries
all 16 caller-supplied categories, including standalone RandomState.

Historical **version 0.1** measurements, 2026-10-12 in a Linux x86-64 orb,
Lean 4.34.0 native executable; sizes/timings below predate fake state. Use
the command below for today's 0.7 corpus, which includes Fakes, Library and Metadata.
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
| JsonQuery (then Json) | 25000 | 5.094 | 4.532 | 5.846 |
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
        assert result.stdout.startswith(b'(core 0.7 (returned ')
        times.append((time.perf_counter_ns() - start) / 1e6)
    print(path.stem, path.stat().st_size, statistics.median(times), min(times), max(times))
PY
```

## Extending the contract

The codec describes today's Lean constructors, not primitives planned for
later streams. Unicode16's interface and trusted boundary, the actual PRNG,
folder entries, the seven fake constructors and the numeric operations
listed above are included in 0.4.

To add a primitive, first add its Lean operation constructor, signature,
evaluator and safety proof. Add its exact constructor-named S-expression
with explicit fields to both codec directions, the table/schema here and
the guards (including a case not already covered by samples). State changes
need new run/report examples too. Do not silently reinterpret RandomState,
Boolean `ro`, root, fake index, or a constructor's field order.

Any added constructor or changed shape/meaning advances the minor wire
version (next `0.8`), updates both Lean and C and their differential corpus,
and adds tests for rejecting unsupported versions. The current parser
accepts **only `0.7`** and rejects unknown constructors; there are no ignored
extension fields or fallback operation names. Support for older versions,
if wanted, must be explicit. This document, not the informal lowering
notation or Lean's `Repr`, specifies what the C implementation must write.
