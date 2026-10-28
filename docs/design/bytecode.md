# Mo Bytecode v0.2

**Implementation contract, checked 24 October 2026 against main at
[`acb2b84`](https://github.com/robertguss/mo/commit/acb2b8488f872c8d907184f2618c467dd8209afc).** This is the
contract between Mo's compiler and its virtual machine, part of Architecture
v0.1 (`architecture.md`). It fixes the file format, the instruction set, the
representation of values, the calling convention and the rules the verifier
enforces. `src/codegen` writes this format through `src/bytecode`'s writer.
The loader, verifier, text tools and VM are integrated. `mo emit FILE -o OUT`
writes a binary file; `mo check --emit=bytecode` prints its indexed text form.
`mo run` compiles source and runs the resulting image, but no CLI command loads
an emitted `.bc` file (D271). This document does not change the format version.

Four files carry the contract as code, and nothing else defines it:

- `src/heap/types.h`: the runtime type table, the layouts, and
  `mo_case_shape` (section 2.3). The compiler, the verifier, the heap, the
  primitives and the host all use it.
- `src/bytecode/bytecode.h`: the file's structures, and the opcodes with their
  operands.
- `src/base/prims.def`: every primitive operation, with its Mo signature, the
  ownership of each parameter and the bugs it can raise (section 6).
- `src/base/hostops.def`: every capability operation, in the same form
  (section 7).

The checker, the code generator, the verifier and the VM all read these files.
None of them keeps its own copy of the information.

## 1. Principles

- **Every register and every field has a static type.** Core is monomorphic,
  so the compiler knows the type of everything. The interpreter never tests
  what kind of value it holds. The one exception is the tag of a choice
  value, which is data.
- **Ownership is explicit.** A register that holds a reference either owns
  it or borrows it, and every instruction says which of its operands it
  consumes. The verifier proves that every owned reference is consumed
  exactly once on every path, and that nothing is used after it is consumed.
  So verified bytecode can't free memory twice, use freed memory or leak it,
  whatever compiler produced it. Memory safety rests on the VM, the
  primitives, the host and the verifier, not on the code generator.
- **The same Core gives the same bytes.** Tables are ordered by a
  deterministic traversal, and nothing in the file depends on addresses,
  times or hash-table order.

## 2. Values

### 2.1 Types

The file's type table lists every type the program uses after
monomorphization, each once. A type is one of:

| Kind | Parameters | Representation |
| --- | --- | --- |
| `unit`, `bool`, `byte`, `int`, `float`, `char`, `time`, `duration` | none | immediate, 8 bytes or fewer |
| `decimal` | none | immediate, 16 bytes |
| `range` | none | immediate: two `int`s |
| `text`, `bytes`, `bigint` | none | reference |
| `tuple` | element types (2 or more) | reference |
| `list`, `set` | element type | reference |
| `map` | key type, value type | reference |
| `record` | name, field names and types | reference |
| `choice` | name; cases, each with a name and its fields' names and types, and whether its one field is unnamed | see 2.3 |
| `function` | parameter types, result type | reference to a closure |
| `env` | the closure code's function index | the closure seen from inside (section 5.3) |
| `capability` | which: `Args`, `Env`, `Console`, `Files`, `ReadFiles`, `Clock`, `Random` or `Simulation` | reference |

Structural types (tuples, lists, sets, maps and functions) are interned, so
two types are equal exactly when their indices are. Records and choice types
are nominal, and each instantiation is its own entry: `Page(Int)` and
`Page(Text)` are two records named `Page(Int)` and `Page(Text)`. A record or a
choice type may refer to itself, directly or through other types, as
`Json` does. A structural type can't contain itself except through a record or
a choice type.

Names of types, fields and cases are kept in the type table, with a mark on a
case whose one field is unnamed, as in `Some(3)`. So the VM can print any value
in its structural text form without running Mo code (section 8).

A type is a **reference type** when its values are pointers, and a choice type
is one when any of its cases has a `ref` or `boxed` shape (2.3). Every other
type is an **immediate type**.

### 2.2 Cells

Every register, field and collection element is a 16-byte **cell**. An
immediate is stored in the cell. A reference is a pointer in the cell's first 8
bytes, with the other 8 zero.

| Type | Cell contents |
| --- | --- |
| `unit` | zero |
| `bool` | 0 or 1 in byte 0 |
| `byte` | byte 0 |
| `int` | a signed 64-bit integer |
| `float` | an IEEE 754 binary64 |
| `char` | a Unicode scalar value, 32 bits |
| `time` | signed 64-bit microseconds since 1970-01-01T00:00:00Z, always within the years 1 to 9999 |
| `duration` | signed 64-bit microseconds |
| `decimal` | a signed 128-bit count of 10⁻¹⁸, within ±(2¹²⁷ − 1) |
| `range` | start and end, two signed 64-bit integers |

Uniform cells keep the VM simple: a register move is one 16-byte copy, and
generic code needs no size information. A `List(Int)` uses 16 bytes per
element. Packing lists of small immediates is an optimization for later,
measured first (Architecture section 13).

### 2.3 Choice values

A choice type whose cases have no fields, such as `Ordering`, is an immediate:
the case's index, from 0 in declaration order, in the cell's first 4 bytes.

Any other choice type's cell holds the tag in its first 4 bytes and a
**payload** in bytes 8 to 15. The payload's shape depends only on the type and
the case. `mo_case_shape(types, type, case)` in `src/heap/types.h` computes it,
and every part of the toolchain calls that one function:

| Shape | When | Payload |
| --- | --- | --- |
| `empty` | the case has no fields | zero |
| `inline` | one field, of an immediate type of 8 bytes or fewer (`unit` to `duration`, or a choice type with no fields) | the field's value |
| `ref` | one field, of a reference type other than a choice type | the field's pointer |
| `boxed` | any other case: several fields, or one field of `decimal`, `range` or a choice type with fields | a pointer to a **case block**, a heap object holding the fields |

So `None`, `Some(3)`, `Some("x")` and `Leased(until: t)` allocate nothing.
`Error(InsufficientFunds(short_by: 0.01))` allocates one case block, for the
`Decimal`. Whether a choice cell holds a reference depends on its tag. `dup`
and `drop` read the tag and look up the shape in a table per type. That's a
table lookup, not a type test.

### 2.4 Heap objects

A heap object starts with an 8-byte header: a 32-bit reference count and a
32-bit layout index. The layout table gives each fixed-size object's fields
and their types, so the VM knows which are references. Records, tuples, case
blocks and closures have fixed layouts; a case block's layout names its
choice type and case. Texts, bytes, big integers, lists, maps, sets,
capabilities and fake capability state have built-in layouts. The VM alone
decides their memory layout, which isn't part of this format.

A count of `UINT32_MAX` marks an **immortal** object. Constants,
capture-less closures and module constants, with everything they reach, are
immortal.

- `dup`, `drop`, `reset`, `set_global` and every primitive leave an
  immortal object's count unchanged, and never free it.
- An immortal object never counts as having one holder. So reuse and in-place
  updates copy it instead of changing it, and processes can share it.
- A `dup` that would bring a count to `UINT32_MAX` makes the object immortal
  instead. It then leaks, but it is never freed early.

Unit tests in `tests/test_heap.c` pin all three; no runtime check asserts them.

The C heap keeps a charged private prefix before the eight-byte header:
teardown links, an iterative-walk link, the allocation size, one structural
hash cache, type, length and a list's live start, and cache/initialization
flags. On 64-bit hosts the prefix is 56 bytes; a two-field `(Int, Int)` tuple
therefore charges 96 bytes (56 + 8 + 32), not just its 40-byte header and fields.
Table-only capacity, size and index metadata lives in its payload, not every object.
These are requested allocation bytes; system allocator bookkeeping is not
included. Fixed-layout cells immediately follow the object header. Built-in
layouts use `UINT32_MAX - mo_type_kind`, outside the file's layout indices.
No native allocation bookkeeping enters a bytecode file. Heap teardown
reclaims all allocations even after a stop or saturation.

A list's live elements occupy `[start, start + length)` within its cell
buffer. The start uses former padding in the private prefix, so neither
the object header nor any fixed layout or charged prefix size changes.
Single-holder slicing releases discarded references and advances the window
without moving its retained suffix. Past half the buffer's capacity, it
compacts the live cells; below one quarter capacity it attempts exact
contraction, transferring ownership and keeping the old buffer on refusal.
Thus repeated single-holder `drop(1)` (or `remove_at(0)`) has amortized constant
storage work, plus destruction of discarded references. Immediate elements
need no discard walk. Shared and immortal updates instead copy just the
retained live elements. Every builder, generic walk and borrowed view reads
the live window, never dead prefix cells; mutations invalidate the hash cache.
Append uses tail slack, and prepend can reuse a dead prefix cell.

The heap-only `mo_heap_new(image, limits)` context is separate from the VM's
process. `mo_heap_image_new(types, limits)` prepares shared abilities, layout
indices, secret domains and an initialization heap once per run. Types are
finished and borrowed for its lifetime; layouts may still be appended during
initialization. Refresh the index before publishing the image to concurrent
processes. Boxed-case and closure builders use the sorted index. There is one
closure layout per function; ambiguous C tables refuse image preparation.
Capture-less closures are initialized once in the image heap, never cached
in a process heap. Immortality preparation computes reachable hash caches
before freezing them; failure leaves counts and immortality unchanged.
Fake state uses the private layout `UINT32_MAX - 32`, not a source type.
It holds typed mutable slots of ordinary values. Capability storage borrows
the host's static operations table, owns its state and optional Text prefix,
and takes a native handle only after successful allocation. The host supplies
its release callback; final drop and heap teardown each release a handle
exactly once, without accessing children or reentering the heap.

## 3. The file

All integers are little-endian. A file is a header, then ten required sections
and an optional services section in this order. Each section is a 32-bit count
followed by its entries.

```text
header      magic "MOBC", format version (major and minor, 16 bits each),
            flags (32 bits, 0 or 1), checksum (FNV-1a, 64 bits, of
            everything after the header)
strings     UTF-8 strings: names, text constants, and the text of tests and
            example lines
types       the type table (2.1)
layouts     fixed-size layouts: kind (record, tuple, case block or closure),
            the type and case they belong to, and their field types
constants   typed literals (3.1)
globals     module constants: type, name and module (3.2)
init        the init functions, in the order they run (3.2)
functions   the function table (3.3)
modules     each module's name, its exposed types and functions, and its
            constants (3.2)
files       exact nonempty/non-NUL filename bytes, not UTF-8 strings (3.4)
debug       spans, declaration/condition locations and register names (3.4)
services    service descriptors, present only when flags = 1 (3.6)
```

Flag value 1 announces a nonempty services section after debug. No other flag
bits are accepted. Files without services retain flags = 0 and have no extra
count word; their encoding is unchanged. Older v0.2 loaders reject flags = 1
rather than silently ignoring service metadata. The format version stays 0.2.

The checksum detects truncation and corruption. It's no defense against an
attacker: Phase 5 signs upgrade packages separately. Every count and index has
a limit in `src/base/limits.h`, and the loader checks every one before
using it.

**Entry encoding.** After the 20-byte header, every count, index, enum and
boolean is a little-endian `u32`, except literal payload bytes and the `u32`
code words. Booleans are 0 or 1. Arrays are a count followed by entries; there
is no alignment padding, section directory, entry length or trailing data.
Strings are a byte length followed by UTF-8 bytes, without a terminator.
Filenames use the same length/bytes encoding but permit arbitrary non-NUL
bytes. There are at most `MO_LIMIT_FILES` rows, each at most
`MO_LIMIT_STRING_SIZE` bytes. They are metadata, not runtime Bytes constants.
Binary and both text codecs accept only version 0.2, rejecting 0.1 and any
other major/minor; there is no compatibility inference from section contents.
`UINT32_MAX` (`ff ff ff ff`, `MO_BYTECODE_NO_INDEX`) marks an absent optional
index, never a table entry. Type and layout entries use the per-kind forms
below, not the memory representation of `struct mo_type` or `struct mo_layout`.
The other sections follow the field order documented in `bytecode.h`.

**Structural decoding.** The private decoder in `src/bytecode/io.h` copies
the tables into a caller-owned arena. It checks this section before the later
verifier checks control flow, operand types and ownership. Instruction widths,
operand index bounds and instruction boundaries are structural too: debug
ranges and jump offsets must be safe to inspect before dataflow. Field and
case operands have their format limits checked here; their compatibility with
the operand's type is a verifier check. Passing decoding alone never permits
execution.
`mo_load` decodes and verifies before publishing an image. The writer performs
both checks by loading its encoded output.
Working-storage ceilings derive from wire size and native entry sizes,
including arena overhead. Tables and scratch arrays are allocated to their
counts, and names are shared with the strings table. Private decoder callers
choose their arena budget; `mo_load` bounds its
image and whole-program scratch arenas by `mo_bytecode_work_budget(wire_size)`.
Function verification has the additional work and register-block ceilings in
section 9. Allocation failure reports zero with a no-memory reason. A decoded
file is independent of the input bytes. Writer and loader errors carry a stable
kind (`INVALID`, `LIMIT`, `NO_MEMORY` in `bytecode.h`); callers do not classify
failures by their human-readable reason text.

Error offsets are byte offsets. Local wire checks report the start of the bad
word (including a count or an index checked while reading); malformed UTF-8
reports the string payload's start. Truncation reports the missing word or
literal payload's start. Instruction-shape errors report the head word;
operand errors report the operand word. Debug errors report the array item's
start, or the declaration/subject/expectation/text/count field. Cross-table
type, layout, signature and module checks report the payload's start (20).
Header size/magic errors report zero, major/minor versions 4/6, flags 8 and
checksum 12. Trailing data reports the first extra byte. Reasons distinguish
an allocation failure from a bad header at zero.

**Type entries.** An entry starts with its `mo_type_kind` number, followed by
only the payload in this table. All type references are indices into this
file's type table; forward references are allowed, including nominal recursion.
The numbers below are the enum values in `heap/types.h`.

| Kind (number) | Payload after kind, in order |
| --- | --- |
| `unit` (0), `bool` (1), `byte` (2), `int` (3), `float` (4), `char` (5), `time` (6), `duration` (7), `decimal` (8), `range` (9), `text` (10), `bytes` (11), `bigint` (12) | none |
| `tuple` (13) | element count, that many type indices; 2 to `MO_LIMIT_FIELDS` |
| `list` (14), `set` (15) | parameter count (exactly 1), element type index |
| `map` (16) | parameter count (exactly 2), key type index, value type index |
| `record` (17) | name string index, field count, that many fields |
| `choice` (18) | name string index, case count, that many cases |
| `function` (19) | parameter count, that many type indices, result type index; at most `MO_LIMIT_BYTECODE_PARAMETERS`, without `env` |
| `env` (20) | closure code's function index |
| `capability` (21) | `mo_capability`: Args = 0, Env = 1, Console = 2, Files = 3, ReadFiles = 4, Clock = 5, Random = 6, Simulation = 7 |

A **field** is its name string index, then its type index. A **case** is its
name string index, field count, then its fields in declaration order. Only
the one unnamed field of a case may have an absent name. Record fields and
all type/case names must name nonempty strings without embedded NUL. The
loader adds NUL terminators to decoded names. All name indices in these type
entries use the lowest string index with the same bytes; the writer finds it
by content and refuses a name missing from the supplied strings table.
This keeps names deterministic even if a string's bytes occur more than once.
Field names are unique within each record or case; case names are unique
within each choice. A choice has at least one case. Names in different nominal
declarations need not be distinct.

No pointer, inactive member, or cached boolean is written. `defined` becomes
true once the complete entry is decoded; incomplete builder declarations are
not files. `has_fields` is derived from the cases' field counts, `reference`
from section 2.3's shapes, and `unnamed` from a case having exactly one field
whose name is absent. No other absent field name is valid. The writer derives
these facts from the active data and ignores the cached booleans. The loader
rejects unknown kinds, invalid names/counts/indices, duplicate interned types
and cycles made only of structural types. Equal nominal names still do not
merge declarations.

**Layout entries.** An entry starts with its `mo_layout_kind` number. Only
the listed fields are written; the field type list always has its own count.

| Kind (number) | Payload after kind, in order |
| --- | --- |
| `record` (0) | record type index, field count, field type indices |
| `tuple` (1) | tuple type index, field count, field type indices |
| `case` (2) | choice type index, case index, field count, field type indices |
| `closure` (3) | function type index, closure code's function index, capture count, capture type indices |

Field/capture counts are at most `MO_LIMIT_FIELDS`. In decoded layouts,
`case_index` is `MO_NO_INDEX` except for case blocks, and `function` is
`MO_NO_INDEX` except for closures; these inactive indices are not written.
Layout fields must match the record, tuple, boxed case or closure captures
exactly, in order. Built-in heap layouts are never entries in this section.
Exact duplicate layouts are refused: each record, tuple, boxed case or closure
function/type pair has at most one fixed layout.

**A worked encoding.** The following are the complete strings, types and
layouts sections, without the header or the other six sections. Each group of
four hex bytes is one little-endian `u32`; comments and line breaks are not
bytes. The types are `Unit`, `Int`, `Text`, recursive nominal `Chain`,
`List(Chain)`, `(Int, List(Chain))`, `fn(Chain) : Int` and `env` for closure
function 0. `Chain` has `End`, `Link(value: Int, next: Chain)` and
`Alias(Chain)`. Both nonempty cases are boxed. Function 0, in the omitted
functions section, has one explicit `Chain` parameter, an owned `env`, returns
`Int`, and captures `Chain` and `Text`.

```text
06 00 00 00                                      ; six strings
05 00 00 00 43 68 61 69 6e                       ; s0 = "Chain"
03 00 00 00 45 6e 64                             ; s1 = "End"
04 00 00 00 4c 69 6e 6b                          ; s2 = "Link"
05 00 00 00 76 61 6c 75 65                       ; s3 = "value"
04 00 00 00 6e 65 78 74                          ; s4 = "next"
05 00 00 00 41 6c 69 61 73                       ; s5 = "Alias"

08 00 00 00                                      ; eight types
00 00 00 00                                      ; t0: unit
03 00 00 00                                      ; t1: int
0a 00 00 00                                      ; t2: text
12 00 00 00 00 00 00 00 03 00 00 00             ; t3: choice, s0, three cases
01 00 00 00 00 00 00 00                         ; End: s1, no fields
02 00 00 00 02 00 00 00                         ; Link: s2, two fields
03 00 00 00 01 00 00 00                         ; value: s3, t1
04 00 00 00 03 00 00 00                         ; next: s4, t3 (recursive)
05 00 00 00 01 00 00 00                         ; Alias: s5, one field
ff ff ff ff 03 00 00 00                         ; unnamed, t3 (recursive)
0e 00 00 00 01 00 00 00 03 00 00 00             ; t4: list, one parameter t3
0d 00 00 00 02 00 00 00 01 00 00 00 04 00 00 00 ; t5: tuple(t1, t4)
13 00 00 00 01 00 00 00 03 00 00 00 01 00 00 00 ; t6: function(t3) -> t1
14 00 00 00 00 00 00 00                         ; t7: env for function 0

04 00 00 00                                      ; four layouts
02 00 00 00 03 00 00 00 01 00 00 00             ; L0: case, t3, Link (case 1)
02 00 00 00 01 00 00 00 03 00 00 00             ; two fields: t1, t3
02 00 00 00 03 00 00 00 02 00 00 00             ; L1: case, t3, Alias (case 2)
01 00 00 00 03 00 00 00                         ; one field: t3
01 00 00 00 05 00 00 00 02 00 00 00             ; L2: tuple, t5, two fields
01 00 00 00 04 00 00 00                         ; t1, t4
03 00 00 00 06 00 00 00 00 00 00 00             ; L3: closure, t6, function 0
02 00 00 00 03 00 00 00 02 00 00 00             ; two captures: t3, t2
```

**Writing and reading.** `mo_bytecode_write` borrows a decoded `mo_bytecode`
and writes fully verified bytes to an initialized, empty `mo_buffer`. It
preserves all table order and indices, never adds strings, and regenerates the header
and checksum rather than trusting the decoded header. It ignores the
constant's decoded-only `encoding`, selecting its payload by type. On invalid
data, a format/output limit or no memory, it returns false with a static
`mo_write_error.reason`, and leaves the buffer empty; capacity may have grown.
Inputs are unchanged and must not alias output storage. C callers supply
readable arrays for nonzero counts and complete nominal definitions. The
caller owns the buffer and frees it with `mo_buffer_free`.

`mo_load` copies, decodes and verifies bytes into an image. The read-only
`mo_image_bytecode(image)` exposes all decoded tables until `mo_image_free`,
including across initialization and calls. **Indices remain the file's** in
both decoded and runtime tables. Any scratch interning used to validate types
must map back to those indices before publishing the image. Tools can read
function kinds, parameter types and ownership, debug text and module exports
without knowing the private image structure. An exposed type entry is its
type index followed by a `u32` `contents_exposed` boolean; generators must
honor it even though the image contains the full definition (Architecture 10.1).

### 3.1 Constants

Each constant has a type and a value:

- a `unit`, `bool`, `byte`, `char`, `int`, `float`, `time` or `duration` in 8
  bytes, and a `decimal` in 16;
- a `text` as a string index;
- `bytes` as a length and the bytes;
- a `bigint` as a `u32` sign (ZERO = 0, POSITIVE = 1, NEGATIVE = 2), a `u32`
  byte length and little-endian magnitude bytes. Zero has sign ZERO and no
  bytes; nonzero has sign POSITIVE/NEGATIVE and a nonzero final byte;
- or a capture-less closure, as a function index.

Unit must be zero; Bool must be 0 or 1; Byte and Char must have zero high
bytes. Signed payloads are two's complement. Every binary64 bit pattern,
including infinities and NaNs with distinct payloads, is accepted and retained;
finiteness is a source-literal rule, not a wire-format rule. Time's bounds are
−62135596800000000 inclusive and 253402300800000000 exclusive microseconds.
Decimal rejects only the two's-complement encoding of −2¹²⁷. Range and choice
constants have no encoding; instructions construct them.

The loader checks each one. Texts are valid UTF-8, and a `char` is a Unicode
scalar value: below 0x110000 and not a surrogate. Times are within the years 1
to 9999, and decimals within range. A closure constant names a function of
kind `closure` with no captures, whose parameter and result types are those of
the constant's function type. The loader turns references into immortal
objects.

### 3.2 Globals and initialization

A module constant is a **global**, computed once by an init function (kind
`init`, section 3.3). The `init` section lists the init functions in the order
they run: the order of the constants' dependencies across the whole program,
with ties broken by module order, then source order. Each init function
computes one global and stores it with `set_global`.

The init functions run once per image, in **the image's initialization**
(`mo_image_init`, Architecture section 8), before any process is made. The
image keeps the memory they allocate for as long as it lives, and
`set_global` makes each value immortal, with everything it reaches. So every
process reads the same globals, and freeing a process never touches them. A
stop during initialization is "a constant that stops stops the program before
it starts" (reference 10.4). `set_global` is valid only during initialization.
Reading a global that isn't set is an internal error: the VM stops with a
report that says so. A correct compiler never causes it.

### 3.3 Functions

Each function has:

- a name and its module (3.5);
- a kind, one of the seven below;
- whether it is **spec code**: a `test`, `example`, `promise` or `anchor`
  function, or the code of an anonymous function written inside one;
- its parameters, each with a type and an ownership, `owned` or `borrowed`;
- its result type;
- for a `closure` function, the types of the values it captures, which make
  the layout of its closure object;
- its registers' types. Registers 0 to p − 1 are the p parameters, and a
  closure's code has one more, of type `env`: the closure itself (5.3);
- its code: 32-bit words.

The kinds, and the signatures of all but the first two:

| Kind | Signature | What it is |
| --- | --- | --- |
| `function` | any | a function of the program |
| `closure` | any, then `env` | the code of an anonymous function, or of a named function used as a value |
| `init` | `fn() : Unit` | computes one module constant and ends with `set_global` |
| `test` | `fn() : Unit` | one test; a failed `assert` executes `fail` |
| `example` | `fn() : Unit` | one example line; when it's false, `fail` gives both sides of a `==` in their text forms |
| `promise` | `fn(p_1, …, p_n, result) : Int` | a promise's whole body: 0 when every line holds, or the number of the first line that doesn't |
| `anchor` | `fn() : Int` | one `allows` or `forbids` line: builds its values and returns the promise function's answer for them |

`mo check` calls every anchor: an `allows` anchor must return 0, and a
`forbids` anchor anything else. `mo test` calls every `test` and `example`
function, and calls each `promise` function on generated inputs (Architecture
section 10.1). Functions of every kind but `function` have no borrowed
parameters.

The source checker allows at most `MO_LIMIT_PARAMETERS` (255) parameters and
reports E1009 beyond it. Runtime function types and bytecode functions allow
`MO_LIMIT_BYTECODE_PARAMETERS` (256). A promise for a 255-parameter subject
has 256 explicit parameters: the subject's parameters, then its result.
A closure has at most 255 explicit parameters plus its implicit owned `env`,
for 256 total. The encoded parameter array and function type omit `env`;
the register type array includes it at index `parameter_count`. The verifier
checks the total including `env`, the leading register types against the
explicit parameters, and the `env` type's function index against the closure.
Direct call/tail-call operand tails allow 256 arguments; closure call tails
allow 255, since the VM supplies `env`. Primitive and host tails use the
bytecode bound and must also match their actual signatures.

### 3.4 Debug information

For every function there is:

- a table from code offsets to spans (file, start byte and end byte);
- for each register that holds a source variable, its name and the code range
  where it holds it, since a register can hold different variables at
  different times;
- for a `test`, its sentence, and for an `example`, `promise` or `anchor`, the
  text of its line (the header for a promise);
- its declaration span, for `mo test --list` and result locations;
- a promise's ordinary subject function, or an anchor's promise, by function
  index, and an anchor's `allows`/`forbids` expectation;
- a promise's condition lines, each with a span and verbatim text, in source
  order. Its result n > 0 selects condition n − 1.

File names are exact byte strings in the separate `files` section. The VM
needs debug metadata to report bugs, and Phase 4 needs it to show a running
program.

**Provenance.** Lowering fills the compiler metadata in Core's program
document (`formal/TEXT.md`); code generation never consults the checked
program or reopens source. The compiler maps expression-preorder spans to
emitted instruction offsets and binder identities/original names to register
ranges after allocation. Lambda lifting carries these tables into closure
code. Generated instructions use their responsible expression/declaration
span. Constants' metadata supplies init names/spans; full function/closure
names are supplied by lowering, numbered in source order (3.5). Core's file
indices are `mo_sources` indices; the compiler copies `metadata.files` to
the separate filename-byte table in original source order, including unused
and duplicate rows. All three debug location kinds retain those indices and
exact bytes. No source filename is normalized, converted to Text or reopened.
Module metadata supplies exports, `contents_exposed` and all global owners.
Complete Core data declarations already supply structural type/field/case
names; no second surface type table is required.

**Encoding.** Each debug entry is: span array, register-name array, text
string index, declaration file/start/end, subject function index, anchor
expectation (`NOT_ANCHOR = 0`, `ALLOWS = 1`, `FORBIDS = 2`), condition-line
array. Each condition entry is file/start/end/text; files index the filename
table and texts index ordinary strings. An absent declaration/condition file is `MO_BYTECODE_NO_INDEX`, with
start/end zero. Code-span entries use that same sentinel to clear source
attribution until the next entry, rather than inheriting the previous source
span for genuinely source-less instructions. All other spans have valid
files and `start ≤ end`. Every
array, and their total, obeys `MO_LIMIT_DEBUG_ENTRIES`. The dedicated filename
section advances bytecode to 0.2; Core's wire version advances independently.
Core 0.8 direct C metadata accepts raw paths, but its UTF-8 text printer
cannot yet emit raw-byte filenames. This bytecode change does not repair that
separate Core text seam.

Core's explicit `noSource` form has empty span/name arrays, absent text and
declaration span, no links/conditions and `NOT_ANCHOR`; no exports are
invented. Tools requiring subject/anchor metadata report it unavailable,
rather than guessing from function names. They can still call selected
tests/examples. A supplied subject must have the appropriate kind; only a
promise may have condition lines, and only an anchor may have an expectation.
With a subject present, a promise has at least one condition and an anchor
has ALLOWS or FORBIDS. Metadata validity does not make a promise true.
The assembler/disassembler preserves every new field, including absent ones.

The debug section's count equals the function count, including empty entries.
Its span, register-name and condition-line arrays together, across all
functions, have at most `MO_LIMIT_DEBUG_ENTRIES` entries. Each entry occupies
16 wire bytes, so the bytecode-size limit currently enforces a tighter
aggregate bound. Code spans have
strictly increasing instruction offsets. Every present source span has a
nonempty NUL-free file name and `start <= end <= MO_LIMIT_FILE_SIZE`; an
absent file uses `MO_BYTECODE_NO_INDEX` with start/end zero, including a
code span that clears attribution.
The loaded image owns all filename bytes independently of the input buffer.
Defect rendering preserves valid UTF-8 scalars, escapes controls and literal
backslashes, and spells each invalid byte as `\xhh`; path newlines cannot
become report separators. Thus byte FF and literal ASCII `\xff` remain distinct.
Register-name ranges are nonempty, half-open word ranges, with both endpoints
at instruction boundaries (the end may equal the function's code length).
Ranges of one register never overlap; array order itself is unrestricted.
Register names, function/global names and module names are nonempty and
NUL-free. Spec text may be absent (`MO_BYTECODE_NO_INDEX`) for source-less
Core; when present it is a string index on a test, example, promise or anchor.
Other function kinds have `MO_BYTECODE_NO_INDEX`, including spec closures.
Condition text is a required string index; empty spec text is allowed.
Subject indices, when present, link only a promise to an ordinary function
or an anchor to a promise. A linked promise has at least one condition; a
linked anchor has ALLOWS or FORBIDS. Other kinds have no subject; conditions
belong only to promises and expectations only to anchors.
Only names in type entries require the lowest matching string index; other
string indices are retained as supplied.
Module global arrays contain every global exactly once, in its owning module.
Each module exposes a type at most once, even with different `contents_exposed`
flags. Exposed functions belong to the exposing module. The init array has one
entry per global and names every init function exactly once; which global
each function sets is checked by the verifier, not the decoder.

### 3.5 Names

A function's name is its module and its source name, as `Wc.count`. An
instantiation of a generic function adds its type arguments, as
`List.map(Int, Text)`. Other kinds are named after what they come from, with a
number counted from 1 in source order: `Wc.test.2`, `Wc.count.example.3`,
`Account.transfer.promise.1`, `Account.transfer.promise.1.allows.2`,
`Account.transfer.promise.1.forbids.1`, `Kv.max_ttl.init`, and
`Wc.count.fn.1` for the first anonymous function in `Wc.count`.

A service's start value is `Module.%service.start` and its timer intervals are `Module.%service.interval.1`, `.2`, … in `every` clause order. `%` marks a compiler-generated name that no source can spell; these names are internal and may change.

### 3.6 Services

An image carries a descriptor for every service module in the program, including imported ones. Only the entry module's descriptor describes the service that running the program starts; the others are retained but never started (reference 11.5).

The optional services section contains at most `MO_LIMIT_SERVICES` descriptors,
one per declaring module. Its order follows checked module order. Each entry
has these fields, in wire order:

| Field | Encoding |
| --- | --- |
| module, state type, start function, token variable | four `u32` indices into modules, types, functions and strings |
| tools | count, then entries: name string index, handler function index, read-only boolean, argument-type index array, result-type index, description string index, parameter-name string-index array |
| timers | count, then pairs of handler function index and interval function index |

Tools and timers retain clause order. Together they contain at most
`MO_LIMIT_SERVICE_HANDLERS` entries; at least one tool is required. Tool names
are nonempty/NUL-free and unique by bytes. Handler indices are unique across
both lists. The token string is the environment variable's name, never its
value: nonempty uppercase ASCII letters, digits and underscores, with no
leading digit.

Every referenced function belongs to the declaring module and has kind
`function`, not spec code. The start function is `() : State`. It is not a
global initializer: loading, importing, or initializing the image never calls
it. Each tool begins with `(State, Time)`; its schema arguments are precisely
the remaining parameters, in order, and its schema result is the complete
handler result (including any state/reply pair or Result wrapper).

The description joins the selected handler's leading `##` comment lines with
newlines, removing each `##` and one following space if present. Ordinary `#`
comments are excluded. With no documentation comment, it is an empty string.
Parameter names exclude State and Time and retain signature order. The loader
requires a valid description string index and exactly one nonempty, NUL-free
name per schema argument; each name is a string index. A tool occupies 28 wire
bytes plus eight bytes per schema argument, including both array counts.

The loader checks the D313 classification independently of the compiler.
Bare `State` and `Result(State, E)` refuse. Only `(State, Reply)` and
`Result((State, Reply), E)` have read-only = 0; all other results have
read-only = 1. Built-in Result uses a nominal choice name beginning `Result(`,
with unnamed one-field `Ok` and `Error` cases in that order. State, parameters
and results must contain no capability, function or closure-environment type,
including through recursive records, choices and containers.

Each timer handler is `(State, Time) : State`. Its interval function is
`() : Duration`, lowered from the checked positive whole-number duration
literal. It uses ordinary checked duration arithmetic; overflow is a defect
when called, not a wrapped interval. The loader checks the signature, not the
value computed by arbitrary bytecode. The service runtime refuses
nonpositive intervals before scheduling them. No scheduling, token lookup or
state saving occurs in this format layer.

All indices and signatures are checked before an image is returned. A missing
function, wrong schema type, wrong read-only flag or duplicate service module
refuses the entire image. The writer enforces the same rules by loading its
output. Cross-table failures use byte offset 20 and the service entry's error
site for text diagnostics.

Indexed text may append this section after `debug ... end`:

```text
services
  S0 m0 t1 f3 s7 tools [(s8, f0, 1, [], t2, s9, [])] timers [(f1, f4)]
end
```

Indices name existing entries. Service indices are consecutive from `S0`;
booleans use 0 or 1, as elsewhere in indexed text. An absent section means no
services; an empty section refuses. Shorthand has no service declarations.
Disassembly and reassembly retain the service section byte for byte.

## 4. Instructions

### 4.1 Encoding

An instruction is a head word, then its operands, one word each. The head
holds the opcode in its low 16 bits and, for the instructions with a variable
number of operands, that number in its high 16 bits. The number counts only
the variable operands: a call's arguments, a `switch`'s targets. Operands are
register numbers, indices into the file's tables, or jump targets, which are
word offsets in the same function.

`r` is a register, `t` a type, `L` a layout, `k` a constant, `g` a global, `f`
a function, `p` a primitive, `h` a host operation, and `@x` a jump target.

### 4.2 Ownership states

The verifier, and this section, give each register a state at each point:

| State | Meaning |
| --- | --- |
| `undefined` | never written on some path here |
| `value` | holds an immediate: no ownership |
| `owned` | holds a reference that this frame must consume exactly once |
| `borrowed` | holds a reference that something outliving this frame owns: a borrowed parameter, a constant or a global |
| `moved` | its reference was consumed; it can't be read until it is written again |
| `token(L)` | holds memory of layout `L` for reuse, possibly none (4.7) |

A register of a reference type is `owned`, `borrowed`, `moved` or `undefined`,
and a register of an immediate type is `value` or `undefined`. A register of a
choice type with references is `owned` or `borrowed` even while its current tag
has no reference.

Each instruction **reads** some operands and **consumes** others:

- To read a register means it must be `value`, `owned` or `borrowed`, and it
  keeps its state.
- To consume a register of a reference type means it must be `owned`, and it
  becomes `moved`. To consume a register of an immediate type only reads it.
- A reference register an instruction consumes can't appear among its read or
  consume operands twice, even when the other occurrence only borrows it.
  Immediate operands may repeat. A result operand is not a second occurrence.
- An instruction reads and consumes its operands first, then writes its
  result. The result register must not be `owned` or hold a token at that
  point. So `prim r1, List.append, List(Int), r1, r2` is valid: it consumes
  `r1` and then writes it. But `dup r0, r0` on an owned `r0` isn't, because it
  would overwrite an owned reference.
- The result becomes `owned` for a reference type and `value` for an immediate
  type, unless the instruction says otherwise.

For a parameter of an immediate type, `owned` and `borrowed` mean nothing.
Tokens temporarily replace the register's declared type: no ordinary read,
move, duplicate or drop can use them. Consuming a token leaves `moved`, even
in an immediate-typed register; a later ordinary write restores its declared
type's state.

A choice register can also have a **known case**. A `switch` gives it one on
each edge it takes. Any instruction that writes the register removes it, and
`move` doesn't pass it on. At a join, a known case survives only if every
incoming edge has the same one.

### 4.3 Moves, constants and memory

| Instruction | Effect |
| --- | --- |
| `move r_d, r_s` | copies the cell; an owned `r_s` is consumed and `r_d` becomes owned; a borrowed `r_s` stays borrowed and `r_d` becomes borrowed too |
| `dup r_d, r_s` | reads `r_s`, adds one to the count, and `r_d` is owned |
| `drop r_s` | consumes `r_s`: subtracts one from the count, and frees the object at zero, dropping what it holds |
| `clear r_d` | writes `undefined`, removing its known case, whatever its declared type; the usual write rule refuses an owned register or token. Reads nothing; at runtime it does nothing: no cell read, count change or allocation |
| `const r_d, k` | loads a constant; a reference is immortal, so `r_d` is borrowed |
| `global r_d, g` | loads a module constant; `r_d` is borrowed |
| `set_global g, r_s` | only in `init` functions; consumes `r_s` and makes it immortal, with everything it reaches |

Freeing never recurses on the C stack. Dropping a long chain of nested values
uses an explicit work list, and so do equality, hashing, comparison and
printing (section 8).

C callers use `mo_value_dup(process, type, value)` and
`mo_value_drop(process, type, value)` in `heap.h`. The type index is the
image's file index, and the cell must belong to that process or its image.
Dup reads an owned or borrowed cell and returns a new owned cell; drop
consumes one owned cell. Both are infallible, follow section 2.4's immortal
and saturating rules, and are a copy/no-op for immediates. A choice's tag
selects its payload's shape. Copying a C cell's bits does not retain ownership.

`mo_call` consumes owned arguments on every outcome; borrowed arguments
remain the caller's. To retain inputs for a promise, C keeps its original
ownership, passes duplicates to the subject's owned parameters, then passes
owned duplicates of the retained inputs and the subject's owned result to
the promise. It drops retained ownership afterward. A returned result may be
dropped before `mo_process_free`; no cell outlives its process (or image).
On stop/failure, no result is returned and C must not reuse consumed cells.

The C builders borrow input cells and duplicate whatever they keep. Their
successful result is owned; false leaves inputs and output unchanged with
no ownership transfer. `mo_value_bytes` copies an arbitrary byte sequence,
including NUL and invalid UTF-8. `mo_value_bigint` copies a sign and
little-endian magnitude in exactly section 3.1's canonical form; signs are
shared in `base.h` so heap need not include bytecode. Both take a `size_t`
length of at most `MO_LIMIT_VALUE_BYTES`; a byte pointer may be NULL only
for zero length. Invalid signs/magnitudes, lengths past the bound and
allocation/heap-limit failures return false. Source literals and encoded
constants separately use `MO_LIMIT_STRING_SIZE`; runtime values do not.

### 4.4 Arithmetic, comparison and ranges

These work on immediates, so ownership doesn't apply. Each family has one
opcode per type it accepts:

| Family | Types | Bugs |
| --- | --- | --- |
| `add`, `sub`, `mul` | `int`, `decimal` | overflow |
| `add`, `sub`, `mul` | `float` | none: IEEE 754 |
| `div`, `rem` | `int`, `decimal` | division by zero; division overflow (`int` minimum ÷ −1, or a Decimal quotient outside its range) |
| `div`, `rem` | `float` | none |
| `neg` | `int`, `decimal`, `float`, `duration` | overflow (the minimum) |
| `add`, `sub` | `duration` | overflow |
| `scale` | `duration` by `int` | overflow |
| `add_time`, `sub_time` | `time` and `duration` | a result outside the years 1 to 9999 |
| `diff_time` | two `time`s, giving a `duration` | none: it can't overflow |
| `eq`, `lt`, `le` | `int`, `decimal`, `float`, `bool`, `byte`, `char`, `time`, `duration`, field-less choice | none |
| `not` | `bool` | none |
| `make_range`, `range_start`, `range_end` | builds a `range` from two `int`s, and reads its ends | none |

`div` rounds toward zero, and `rem` takes the left side's sign (reference 3.2).
Decimal `mul` and `div` compute exactly in 256 bits and round to the nearest
10⁻¹⁸, an exact tie to even (reference 3.3). `float` comparisons follow IEEE:
not-a-number is unequal to everything. `cmp_float` gives an `Ordering` in which
it sorts last (reference 9.2). `gt` and `ge` are `lt` and `le` with their
operands swapped. Everything else on numbers, such as `pow` and `floor_div`,
is a primitive (section 6). In a family with one opcode per type, each opcode
is named by the family and its type, as `add_int`, `lt_decimal` and
`scale_duration`; the others, such as `add_time`, are single opcodes.

### 4.5 Control

| Instruction | Effect |
| --- | --- |
| `jump @x` | |
| `branch r_c, @t, @f` | `r_c` is a `bool` |
| `switch r_s, @0, …, @n−1` | one target per case of `r_s`'s choice type, in declaration order; no default. On the edge to `@i`, `r_s`'s known case is `i` |
| `call r_d, f, r_1, …, r_n` | consumes each argument passed to an `owned` parameter; only reads those passed to `borrowed` ones |
| `call_closure r_d, r_c, r_1, …, r_n` | consumes the closure `r_c` and every argument (closure parameters are all owned: 5.3) |
| `tail_call f, r_1, …, r_n` | as `call`, then returns the callee's result. Allowed only when every other owned register and every token has been consumed, and every argument of a reference type passed to a `borrowed` parameter is itself `borrowed` |
| `tail_call_closure r_c, r_1, …, r_n` | as `call_closure`, under the same condition |
| `ret r_s` | consumes `r_s`, whose type must be the function's result type. Every other owned register and every token must have been consumed |
| `stop r_t` | a bug: stops with the reason `r_t`, a `text` |
| `fail r_t` | only in spec code: an assertion or an example failed, with the detail `r_t`, a `text`, such as both sides of a failed `==` |

Every path through a function ends in `ret`, a tail call, `stop` or `fail`.
None runs off the end of the code.

### 4.6 Data

| Instruction | Effect |
| --- | --- |
| `construct r_d, L, r_1, …, r_n` | builds a record, tuple or case block of layout `L`, consuming the fields. With a case block's layout, `r_d` is a cell of that layout's choice type, with its case's tag |
| `make_case r_d, t, i, r_1, …, r_n` | builds case `i` of choice type `t` from its fields, which it consumes; boxes them in a case block when the shape is `boxed` |
| `field r_d, r_s, i` | reads field `i` of a record, a tuple, or a closure through its `env`; `r_d` gets a duplicate, so it is owned |
| `case_field r_d, r_s, i, j` | reads field `j` of case `i`; `r_s`'s known case must be `i`; `r_d` is owned |
| `set_field r_d, r_s, i, r_v` | consumes `r_s` and `r_v`; `r_d` is `r_s` with field `i` replaced (below) |
| `make_closure r_d, f, r_1, …, r_n` | consumes the captured values; `f` is a `closure` function, and `r_d`'s type is the function type of its parameters (5.3) |
| `equal r_d, t, r_a, r_b` | reads both; structural equality for a type with `Equal` (reference 9.2), `r_d` a `bool` |
| `prim r_d, p, t, r_1, …, r_n` | a primitive (section 6). `t` is the function type of its signature at its instantiation, such as `fn(List(Int), Int) : Option(Int)` for `List.get`, so the verifier and the VM know the type of every argument and of the result, even for a primitive with no parameters, such as `Set.empty` |
| `host r_d, h, r_c, r_1, …, r_n` | a capability operation (section 7). `r_c` is the capability, which it reads. A fake constructor has no capability to call through, so it is `host r_d, h, r_1, …, r_n`, and only spec code may use it (section 9) |
| `sim r_d, op, D, T, r_1, …, r_n` | opcode 87, VM-handled `SIMOP` family; fixed immediate operation/descriptor/tool indices, all argument registers borrowed. START=0, SEND=1, ADVANCE=2, CRASH=3, RESTART=4. Start takes Time, returns Simulation and is spec-only. Send takes the handle plus descriptor-selected tool arguments and returns Result(complete handler result, SimulationError). Advance takes handle/Duration; crash/restart take handle; controls return Unit. Controls require D=T=0; start requires T=0. The verifier checks all indices, arities and types against service descriptors; no fixed host signature is used. |

`set_field`, and every primitive that "changes" a collection, work the same
way:

- **With one holder,** the object is changed in place. What it replaces, such
  as the old field, is dropped, and the new value is stored. The result is the
  same object.
- **Otherwise,** including when the object is immortal, a new object is made.
  Every part kept from the old one is duplicated, the new value is stored,
  and the old object's count goes down by one. A count above one can't reach
  zero that way, and an immortal one doesn't change.

### 4.7 Reuse

`reset` and `reuse` let a value's memory be reused when it has one holder, as
in Perceus and Lean 4:

| Instruction | Effect |
| --- | --- |
| `reset r_k, r_s` | consumes `r_s`: a record, a tuple, or a choice value whose known case is `boxed`. With one holder, the object's fields are dropped and `r_k` keeps its memory. Otherwise the count goes down by one, unless the object is immortal, and `r_k` keeps nothing. Either way `r_k` becomes `token(L)`, with `L` the object's layout |
| `reuse r_d, r_k, L, r_1, …, r_n` | as `construct`, but builds in the token's memory when there is some. `L` must be the token's layout. It consumes `r_k` and the fields |
| `free_token r_k` | frees the token's memory, if any, on a path that doesn't reuse it |

The C token API has the builders' borrowed-field convention: `reset` consumes
the old value, while `reuse` duplicates the supplied fields and consumes its
token only on success. A failed reuse leaves the token and fields unchanged.
The interpreter uses the owned-field variants, transferring fields without
dup/drop pairs; a broken layout/count invariant asserts, not an allocation stop.
Tokens cannot be read as values; their fields are cleared and own nothing.

## 5. Calls and frames

### 5.1 Frames

Frames live on the VM's own stack, not the C stack. A call pushes a frame of
the callee's register count and copies the arguments into registers 0 to n − 1.
`ret` copies the result into the caller's `r_d` and pops the frame. The stack
has a size limit and the call depth a count limit. Exceeding either is the bug
"out of stack" (reference 13.2), with a report.

The C implementation charges each frame's native header, 16-byte register
cells and one ownership-state byte per register, rounded up to 8-byte
alignment. On the supported 64-bit platforms the header is 72 bytes: three
registers take 128 bytes, five take 160. A frame counts owned cells and tokens
so return/tail-call ownership checks are constant-time. Entry zeroes only the
state bytes, never unreadable cells. Reserved stack capacity, including
growth slack, never exceeds `--max-stack`; metadata and value storage instead
count against `--max-heap`. The root call counts as depth one. Calls reserve
space and depth before transferring any register. Native allocation failure
is "running out of memory", not a fictitious stack/depth limit violation.

### 5.2 Tail calls

A tail call replaces the current frame. The arguments are copied to a scratch
area first, since they come from the frame being replaced. If the callee
needs more registers, the frame grows in place, since it is at the top of the
stack. It adds no frame and no call depth, but a larger callee can increase
stack bytes: earlier frames plus the new top frame must fit `--max-stack`.
Reservation failure leaves the caller and operands untouched. A fixed-size
tail loop uses constant space (reference 5.3).
Every call in tail position is compiled as a tail call, including calls to
closures.

The scratch argument array is a fixed C-stack buffer for at most 256 cells
(256 direct arguments or 255 explicit closure arguments). The environment
travels separately. The buffer is not recursive and is
independent of call depth. Construction gathers up to 16 fields locally;
larger field arrays use checked scratch charged to the remaining heap. A
constructor, field update, reuse or equality refusal stops with "running out
of memory" and preserves every operand, token and destination. The
heap-limit report names `--max-heap`; a native allocator refusal names no
Mo limit. Arithmetic overflow names `Int`, `Decimal` or `Duration`; a Time
outside years 1 to 9999 has its own reason. `reset` and
`free_token` do not allocate. Minimum Int remainder by −1 returns zero,
as in the Lean model; the VM avoids evaluating C's undefined signed `%` for
that pair. Division by that pair still overflows.

A borrowed parameter is kept alive by its owner, an earlier frame. A tail call
removes the current frame, so a reference the current frame owns can't be
passed to a borrowed parameter in a tail call: nothing would release it.
Borrow inference (5.4) makes such a parameter owned instead, and the verifier
checks the result.

### 5.3 Closures

An anonymous function, and a named function used as a value, compiles to a
function of kind `closure`. Its parameters are the function value's
parameters, all `owned`, then one parameter of type `env`, which is the
closure object, also owned. The code reads captured values from it with
`field`, and drops it like any owned register.

`make_closure` builds the closure object from the function index and the
captured values. `call_closure` passes the arguments and then the closure
object itself. A closure that captures nothing is a constant (3.1), so it is
immortal and costs no allocation. All parameters of closure code are owned,
because a call through a function value can't know which closure it will
reach.

### 5.4 Borrowed parameters

Borrow inference follows Lean 4 (Ullrich and de Moura, "Counting Immutable
Beans", 2019). A parameter of a reference type is `owned` if:

- the function returns it, or stores it in a constructed value or a closure;
- the function passes it to an owned parameter, or to a closure;
- the function resets it for reuse;
- **or any caller passes it, in a tail call, a reference that caller owns.**
  So when `report(path, console, files)` is in tail position and `path` is a
  value the caller owns, `report`'s `path` is owned, and `report` drops it.
  The tail call stays a tail call, as reference 5.3 promises.

Otherwise it's `borrowed`. Inference runs to a fixed point over the whole
program's call graph. Functions of every kind but `function` have no borrowed
parameters.

## 6. Primitives

A primitive is a library function written in C: the operations of section 14
of the reference that are arithmetic, conversions, text and byte operations,
or the basic operations of lists, maps and sets. Every function of section 14
that takes a function as a parameter is written in Mo, in `lib/`, so C never
calls back into Mo code. That covers `map`, `filter`, `reduce`, `sort_by`,
`group_by`, `update` and the rest.

`src/base/prims.def` lists the primitives, one line each:

```c
/*   id                  module   name         Mo signature                       params  bugs */
PRIM(LIST_GET,           "List",  "get",       "fn(List(T), Int) : Option(T)",    "bb",   "")
PRIM(LIST_APPEND,        "List",  "append",    "fn(List(T), T) : List(T)",        "oo",   "")
PRIM(LIST_TAKE,          "List",  "take",      "fn(List(T), Int) : List(T)",      "ob",   "negative count")
PRIM(MAP_SET,            "Map",   "set",       "fn(Map(K, V), K, V) : Map(K, V)", "ooo",  "")
PRIM(TEXT_BYTE_COUNT,    "Text",  "byte_count","fn(Text) : Int",                  "b",    "")
PRIM(INT_POW,            "Int",   "pow",       "fn(Int, Int) : Int",              "bb",   "overflow; negative exponent")
```

- **The checker** adds each primitive to its type's module, as if it were
  declared there. A library module can also hold tests of C primitives, as
  `lib/text.mo`, `lib/set.mo` and `lib/unicode16.mo` do, without repeating their
  signatures. Higher-order functions and Json are Mo code in `lib/`.
- **Lowering** turns a call to a primitive into a Core primitive operation.
- **The code generator** emits `prim` with the instantiation type, and treats
  each parameter as owned (`o`) or borrowed (`b`).
- **The verifier** checks the argument and result types against the signature
  at that type.
- **The VM** dispatches on the id to a C function in `src/prim/`.

A primitive that "changes" a collection, such as `append`, `set`, `remove` or
`take`, consumes it and follows section 4.6's two paths. A primitive that only
reads, such as `get`, `size` or `contains?`, borrows. Every row in `prims.def`
has a C implementation. That does not mean every source-level call lowers;
native-call coverage belongs to `src/lower`. MO-116 delivered Env.get/of,
Ordering/Entry/Map text conversion and Ordering comparison (#189), and MO-144
qualified `T.to_text` and `T.compare` for every type with the ability (#208),
using existing Core operations rather than new primitives.
`docs/status/golden-gaps.md` names the remaining `rules.tsv` gap (11.5/4, E1408).

On a stop, primitives and host operations must leave operands in their
pre-instruction state, including their ownership and the objects they name, or
the VM must preserve that state for the defect report before the operation
consumes or changes it. This applies to partial in-place updates followed by
allocation failure and to `Map.%literal_insert` stopping on a duplicate key.
A consumed or freed operand must never be printed from pre-instruction replay.

The interpreter dispatches `prim` through `mo_prim_call` and `host` through
`mo_host_call`, committing owned register consumption only after success.
Both dispatch paths are implemented in `src/vm/execute.c`. The differential
runner (`ci/check_vm.py`) pins unavailable primitive and host dispatch counts
at zero; an unavailable dispatch is not a matched Mo outcome. C-only
instruction budgets bound fuzz execution, including initialization loops.
They are disabled for normal execution and are not language resource limits.

## 7. Capabilities and host operations

A capability is a heap object that carries its own operations. It has:

- which capability it is;
- a pointer to its operations table, real or fake;
- for a fake, an owned reference to the fake's **state**;
- for `Files` and `ReadFiles`, its folder: an open handle for a real one, and
  a path prefix for a fake one;
- and whether it is read-only.

`host r_d, h, r_c, …` calls operation `h` of `r_c`'s table. A fake
constructor makes a capability rather than using one, so `host` calls it by
`h` alone.
`src/base/hostops.def` lists the operations, with the same columns as
`prims.def`:

- `Console`: `print`, `error`, `read_line`, `printed` and `printed_errors`;
- `Files`: `read_text`, `read_bytes`, `write_text`, `append_text`, `remove`,
  `make_folder`, `list`, `entries`, `exists?`, `within` and `read_only`;
- `ReadFiles`: `read_text`, `read_bytes`, `list`, `entries`, `exists?` and `within`;
- `Clock`: `now`. `Random`: `int`. `Args`: `list`. `Env`: `get`;
- the fake constructors: `Console.fake`, `Console.fake_input`,
  `Files.in_memory`, `Clock.fixed`, `Random.seeded`, `Args.of` and `Env.of`.

`STOREOP` entries in `hostops.def` (`LOAD`, `APPEND`, `SNAPSHOT`) are runtime-only:
they have no bytecode opcode and no `host` encoding.

`Files.walk` and `ReadFiles.walk` are Mo functions over these operations in
`lib/files.mo` and `lib/read-files.mo`, not extra host opcodes.

The verifier allows the fake constructors, `printed` and `printed_errors` only
in spec code (3.3), which is where the reference allows them (reference
12.4). Code that isn't
spec code can't call, tail-call or make a closure of a spec function or an
`init` function (section 9, item 6). So `main`, and everything it reaches, can
neither make a fake nor run a test's code, even in an image that contains
tests.

**A fake's state**, a `Random`'s generator state, and real buffered input or
deferred folder bindings are the only mutable objects in the heap. Fake
slots hold numbers or ordinary counted values: texts, bytes, and lists, sets
and maps of them, never a capability or a closure. Native capability storage
is inline, charged heap memory, not an uncharged side table. A deferred real
folder may retain its older parent capability until teardown; those links
always point from a new child to an existing parent, so the heap stays acyclic.
They change only the way programs change values: `Files.in_memory(files)`
consumes its map, so if the test still holds the map, the first write copies
it; `printed` returns a duplicate of the state's list, so printing more later
doesn't change a list the test holds. Every handle on one fake shares its
state: the `Console.fake()` passed to `main` and the one the test reads
`printed` from are the same object. `files.within("a")` and `files.read_only`
make new handles on the same state, with a longer prefix or read-only set.

**Simulation** is a VM-owned fake capability. Its native context owns one
service loop and one open in-memory store. The last reference, or heap
teardown after a stopped test, invokes its release once: free the loop, then
close the store, without accessing the caller's heap. Crash only frees the
loop; restart reuses that same open store. Construction owns the store until
the handle exists, after which only release closes it. The shared store limit
is eight; a test that stops still returns its slot. Arguments and replies are
copied between same-image heaps by the saved codec, never shared by refcount.
Simulation is excluded from main capabilities and service descriptor types.
`SimulationError` is a two-case choice, `ServiceDown` then `ToolFailed`, both
without fields. These names are checked as a built-in shape at send sites.

**A real capability** is made by the runtime, once, for `main`. Dropping a real
`Files` or `ReadFiles` handle closes its folder's descriptor. The number of
open handles has a named limit, and `within` past it is a bug. `mo_call_main`
retains each declared kind in the process so repeated calls share it.
Host refusal keeps the capability and operand ownership intact. Fake updates
are transactional on allocation failure; real external I/O and bindings cannot
be rolled back (architecture 7).

## 8. Generic operations in the VM

The VM implements a few operations for every type, driven by the type table:

- **Equality and hashing** of any type with `Equal` or `Hash`, as `equal` and
  inside the map and set primitives. They are always structural (reference
  9.2), so they never run Mo code. Maps and sets compare their entries or
  elements in any order, and hash them in a way that doesn't depend on order.
- **Dropping** any value (4.3).
- **The structural text form** of any value, for defect reports and Phase 4's
  introspection. It follows reference 9.2 without anyone's own `to_text`, so
  printing a value can never stop: names from the type table; `Some(3)` for a
  case's unnamed field; sets as `[1, 2].to_set`; floats in their shortest
  round-trip spelling, with not-a-number as `nan` and the infinities as
  `inf` and `-inf`; decimals in their shortest exact spelling; times
  in RFC 3339; durations in seconds, as `1.5s`; texts and characters quoted and
  escaped inside other values.

All of them use explicit work lists, never C recursion, so a deeply nested
value can't overflow the C stack. Order and the text forms that programs see,
including customized `compare` and `to_text`, are Mo functions generated by
lowering for each type (Architecture section 4.3), not VM operations.

Float digits use the exact nearest-coefficient search in Lean's
`Support/Num.lean`, including ties to even and adjacent candidates. Fixed
notation applies to decimal point positions -5 through 21 (D236). BigInt
decimal conversion uses balanced radix conversion and iterative Karatsuba,
not quadratic repeated division. Functions and environments print as
`<function>` and `<environment>`; capabilities as `<Files>`, `<Console>` and
the other capability names, without revealing native handles or addresses.
Bytes print as `bytes[0, 255]`. Nested text escapes interpolation openers.

Equality and ordering memoize pairs only when either object has more than
one holder (including immortal counts). Hashing caches one structural hash
per object. Unique trees need no pair memo; shared DAGs do not expand into
exponential walks. Equality, ordering and hashing have no work cap. Scratch
uses the remaining process heap; memo growth releases old arrays. For a type
with the required ability, a failed generic operation means running out of
memory, reported as that bug (reference 13.2), not an arbitrary walk limit.

Runtime Text, Bytes and BigInt use u32 byte lengths, with representation ceiling
`MO_LIMIT_VALUE_BYTES` (4,294,967,295 bytes). `MO_LIMIT_STRING_SIZE` bounds C
source/constant input, not computed values. Runtime allocation and scratch use
the process's `--max-heap`; primitive refusals name it or the representation
ceiling. Balanced BigInt decimal conversion has no magnitude or work cap beyond
available heap and output. Structural printing expands shared edges only as far
as the caller's output buffer allows, with work proportional to emitted text
apart from subquadratic BigInt conversion. Printing can refuse scratch or caller output allocation,
preserving existing output. Scalars and the first 16 frames use the C stack,
so scalars and shallow values still print with a full process heap (BigInt
conversion may require scratch). Defect renderers use `<value unavailable>`
when printing refuses, without another stop or Mo call. Output buffers belong
to the C caller, outside the process heap, with their own explicit byte limit.

SipHash-1-3 uses a secret 128-bit seed obtained with `getentropy` once per VM
run; entropy failure refuses image preparation. Distinct hash and index
domains are derived from it. Structural hashes exposed to programs do not
reveal the index domain; indices mix the structural hash with that domain.
All processes and image tables use these same domains, including COW copies.
Image hash caches are prepared during initialization and never written after
publication. Hashes are never equality proofs.

Indices are at most half full. Lookups and removals probe to an empty slot;
they never refuse merely for crossing `MO_LIMIT_HEAP_PROBES`. An insertion
that would cross that bound grows the index and retries, including during
building and copying. Growth can refuse memory or the explicit index/collection
size limits in `limits.h`, preserving inputs. Copies compact live entries and
size storage from their count; sparse tables shrink at removal. Maps and sets
keep insertion order, so secrets never change program results. (`mo test`'s
generator has its own fixed hash key: Architecture section 10.1.)

## 9. Verification

The loader refuses a file unless the verifier accepts all of it. The verifier
checks:

1. **Structure.** The header, version and checksum; every count and index
   within its limit and its table; every string valid UTF-8; every constant
   valid (3.1).
2. **Types and layouts.** Every type well-formed (2.1); every layout's fields
   matching its record, tuple, case or closure; every choice type with at
   least one case and at most 65,535. Every allocation has its fixed layout;
   unused types need no layout. Every closure constant has its closure layout.
   Map keys and set elements have `Hash`.
3. **Code shape.** Every function's code decodes into whole instructions, with
   the right number of operands for each opcode; every jump target is the
   start of an instruction in the same function; every path ends as section
   4.5 requires. Every instruction is reachable from its function's entry;
   unreachable instructions are refused. Infinite loops are allowed: this
   checks finite paths and fall-through, not termination.
4. **Types of operands.** Each instruction's registers have the types its
   opcode, primitive or host operation requires at its instantiation type;
   calls match their targets' parameter and result types; `ret` matches the
   function's result type; `make_closure` matches its function; `switch` has
   one target per case. `construct` and `reuse` cannot build closures;
   `make_closure` requires its closure layout. `set_field` changes only a
   record or tuple, not a choice or an `env`; `field` may read `env` captures.
   Direct calls cannot target closure code (whose `env` only closure calls
   supply).
5. **Ownership and definition.** A dataflow over the states of section 4.2,
   with known cases, per function. Every read and every consume follows 4.2.
   At every join, each register's state is the same on every incoming edge.
   A code generator makes dead registers' states agree with `clear`, after
   `drop` for an owned reference or `free_token` for a token, including on
   loop back-edges.
   `ret` and tail calls leave nothing owned except what they consume. `stop`
   and `fail` may leave owned registers: they end the process, whose heap is
   freed all at once (Architecture section 8).
6. **Kinds of function.** `set_global` only in `init` functions, and every
   global set by exactly one of them, listed once in the `init` section.
   `fail`, the fake constructors, and `Console`'s `printed` and
   `printed_errors` only in spec code (reference 12.4). Code that isn't spec
   code doesn't call, tail-call, make a closure of, or load a closure constant
   of spec code. No code calls an `init` function: only image initialization
   invokes it. No function of a kind other than `function` has a borrowed
   parameter. Each kind has its signature (3.3).

A promise linked to a subject has the subject's parameter types in order,
then its result type, all owned. Without a subject link there is no signature
to compare; its nonempty parameter array and Int result are still checked.

An init function's stores all name one global, distinct from every other
initializer's global. Dataflow proves exactly one store on each normally
returning path; `stop` may precede the store. A store in a cycle or a second
store on a path is refused. At each global read or call in an init function,
all globals read directly or through direct calls must already be initialized,
including recursive helpers. Unknown closure calls are not charged with every
closure of the same type: unrelated closures may depend on this very global.
The compiler orders actual source dependencies; the VM's unset-global guard
handles dependencies reached through unknown closure calls. Reading its own
global after its store is allowed. The VM still guards unset reads as an
internal error (3.2). Source-order tie breaking is a compiler rule, not a
loader check: the file contains no source-order dependency information.

Primitive and host signatures come only from `prims.def` and `hostops.def`.
Generic variables bind exact type indices consistently, and `Equal`, `Hash`
and `Number` constraints are checked, including recursive nominal types.
Functions, environments and capabilities have neither `Equal` nor `Hash`;
Float has `Equal` but not `Hash`. Other compound types have an ability exactly
when all their parts have it. `Number` is Int, BigInt, Decimal or Float.
The binary format has no marker for a library nominal declaration. For this
ABI, signature matching recognizes library declarations by their exact ordered
case/field shapes, not their printable type names: Option has `None` and
unnamed `Some(T)`; Result has unnamed `Ok(T)` and `Error(U)`; Ordering has
field-less `Less`, `Equal`, `Greater`; FileError has `NotFound(path: Text)`,
`NotAllowed(path: Text)`, `NotText(path: Text)`, and
`Failed(path: Text, reason: Text)`. Their field types still bind file indices.
Entry is a record `(name: Text, kind: EntryKind)`; EntryKind has field-less
`File`, `Folder`, `Link`, `Other` cases. Entry needs its record layout.
This permits a nominal declaration of the same shape at an ABI boundary
without letting a name disguise a different representation.
Tuples and boxed cases appearing explicitly in a primitive or host signature
need their fixed layouts. Generic variables do not impose this requirement
on their bound types: these values are passed or copied, not constructed by
the operation.

Non-spec code can obtain function values only from non-spec closure creation,
non-spec closure constants, its inputs or globals initialized by non-spec
code. Consequently a run starting at non-spec `main` cannot obtain or call a
spec closure, even through containers or ordinary helper parameters. C callers
that explicitly inject spec function values are outside that entry guarantee.

Local type, kind, case and exit refusals point to the instruction head;
read/consume/write refusals point to the offending operand; a join refusal
points to the destination block's first instruction. Whole-table relations
point to payload offset 20. Allocation failures have offset zero.

Ownership visits each basic block once with an explicit depth-first stack and
an undo log of changed registers. Complete vectors are retained only at joins
and loop entries; repeated incoming edges compare with those vectors.
Known-case analysis runs only for choice registers used by `case_field` or `reset`:
each block's fact can change only from unreached to a case/unknown, then to
unknown. Before doing work or allocating vectors, verification enforces
`MO_LIMIT_FUNCTION_REGISTER_BLOCKS` (registers × blocks ≤ 2²²), and charges at
most `MO_LIMIT_VERIFY_WORK_PER_WORD` (128) steps per encoded code/register word
plus 32 header words. Exceeding either ceiling is a load error at the entry
instruction or the instruction where work would cross the ceiling, respectively.
Scratch is bounded by `MO_LIMIT_VERIFY_BYTES_PER_WORD` (2,048) bytes times that
same word count, including arena overhead. Lowering must respect these ceilings.
Unused choice registers and jump chains take linear work; expensive dataflow
is refused within a wire-sized budget, rather than asking for quadratic storage.
Ability propagation and
initialization dependency summaries take linear graph work; fixed-layout
lookups use a sorted index. All graph walks use explicit stacks or queues.

A file that passes runs without use-after-free, double frees, leaks, reads of
undefined registers, out-of-range fields or type confusion, given a correct VM,
primitives and host. Those are the trusted code: the reason condition 4 of D207
keeps them small. A verified file can still stop with a bug; that is Mo's
semantics, not a failure of verification.

When a process stops, the defect report prints the stopping frame's
variables. It prints only registers that are `value`, `owned` or `borrowed` at
the stopping instruction, named from the debug ranges of 3.4. It gets those
states by running the verifier's dataflow again over that one function. So it
never reads a register whose reference was consumed.
The VM-facing `mo_image_registers_readable` wraps the private replay through
the module's `bytecode.h`. It reports states immediately before the instruction, never after
its consumes or writes, and refuses non-boundary PCs.
The image caches abilities and the sorted layout index; replay never repeats
whole-file type checks. Replay uses the same per-function ceilings. The VM,
primitives and host must preserve the pre-instruction operand state on a stop,
as section 6 requires, or supply a saved state to the report. Pre-instruction
readability alone does not make a partially consumed or mutated operand safe.
The private streaming replay visits every instruction boundary in one analysis,
updating the readable mask only for changed registers. It lets fuzzing compare
all boundaries without repeating a whole-function analysis at each instruction.

Stopping records frame views without allocating, top frame first. A caller's
PC stays at the call until its callee returns, so its span names the call site.
Rendering appends at most 64 KiB to the caller's bounded output buffer. It
writes the reason and top frame before optional details. Stacks longer than
eight frames show the top four and bottom four with `… N frames …` between.
Detail text and each variable's output use at most 4096 bytes. Details and
Text variables truncate with a marker; other values that cannot print within
their budget show `<value unavailable>`. Terminal controls, including C1,
are escaped in details, names and nested values. Text and Char variables use
their quoted, nested structural form. Optional rendering failure
keeps the reason/top frame and marks omitted output; even a caller's smaller
buffer gets a partial report, never a rollback of its useful prefix.
The replay mask and verifier scratch are diagnostic storage outside the
exhausted process heap; their own verifier limits still apply. Replay refusal
prints `<variables unavailable>` without reading any cell. A value-printing
refusal prints `<value unavailable>`. No source-name range means no variable
to print; moved/undefined registers and reuse tokens are never printed.

## 10. The text form

Every file has a text form. It is used to write the VM's tests by hand, and
`mo check --emit=bytecode` prints it. There are two forms: **indexed**, which
the disassembler always prints, and **shorthand**, for hand-written tests.
Both assemble to the file of section 3, without changing that format.

### 10.1 Indexed form

Every section occurs once, even when empty, in file order. Entries have
consecutive zero-based indices in their section; forward references are
allowed. Nothing is added, interned, sorted or removed. Arrays retain their
order, including unused entries and repeated strings, constants or layouts
where section 9 permits them. Type names still obey section 3's lowest-string-
index rule; other string references retain the index written.

The grammar below is line-based. Each entry, instruction and keyword line
ends with a newline. `;` starts a comment outside quotes. In the grammar,
`list(X)` means `"[" [ X { "," X } ] "]"`; it is not a text keyword.
`U` is a decimal `u32`. `S`, `T`, `L`, `K`, `G`, `I`, `F`, `M`, `PATH` and `D` are
`s`, `t`, `L`, `k`, `g`, `i`, `f`, `m`, ASCII uppercase `P` and `d` followed by a decimal index.
`R` is `r` followed by a register number; `AT` is `@` followed by a code word
offset, not an instruction number. `OPT_S` is `S` or `-` (absent, not an
empty string); `OPT_F` is `F` or `-`; `OPT_PATH` is `PATH` or `-`. Absent references encode
`MO_BYTECODE_NO_INDEX`, never an index spelled with that numeric value.
All counts and byte lengths are derived from the lists and
payloads. No decoded-only fields are written.

```text
file        = indexed | shorthand
indexed     = "bytecode 0.2 indexed"
              strings types layouts constants globals init functions modules files debug
strings     = "strings" { S Q } "end"
types       = "types" { T type_body } "end"
type_body   = SIMPLE
            | "tuple" list(T) | ( "list" | "set" ) T | "map" T T
            | "record" S list(field) | "choice" S list(case)
            | "function" list(T) T | "env" F | "capability" CAP
field       = "(" OPT_S "," T ")"
case        = "(" S "," list(field) ")"
layouts     = "layouts" { L layout_body } "end"
layout_body = ( "record" | "tuple" ) T list(T)
            | "case" T U list(T) | "closure" T F list(T)
constants   = "constants" { K T payload } "end"
payload     = scalar | "text" S | "bytes" HEX
            | "bigint" SIGN HEX | "closure" F
scalar      = "()" | "true" | "false" | NUMBER | CODEPOINT | "bits" HEX
NUMBER      = [ "-" ] DIGITS [ "." DIGITS ]
CODEPOINT   = "U+" HEXDIGITS
globals     = "globals" { G T S M } "end"
init        = "init" { I F } "end"
functions   = "functions" { indexed_fn } "end"
indexed_fn  = F S M KIND "spec" BOOL
              "params" list(param) "result" T "captures" list(T)
              "registers" list(T) "code" { instruction } "end" "end"
param       = "(" T "," OWNERSHIP ")"
instruction = OPCODE [ operand { "," operand } ]
operand     = R | T | L | K | G | F | P | H | AT | U
modules     = "modules" { M S "types" list(export)
              "functions" list(F) "globals" list(G) } "end"
export      = "(" T "," BOOL ")"
files       = "files" { PATH ( HEX | Q ) } "end"
debug       = "debug" { D "spans" list(span) "variables" list(variable)
              "text" OPT_S "declaration" location "subject" OPT_F
              "expectation" EXPECTATION "lines" list(spec_line) } "end"
location    = "(" OPT_PATH "," U "," U ")"
span        = "(" AT "," OPT_PATH "," U "," U ")"
variable    = "(" R "," S "," AT "," AT ")"
spec_line   = "(" OPT_PATH "," U "," U "," S ")"
EXPECTATION = "none" | "allows" | "forbids"
```

`SIMPLE` is a payload-free kind from section 3 (`unit` through `bigint`).
`CAP` is one of its eight capability names. `KIND` is one of section 3.3's
seven function kinds; `OWNERSHIP` is `owned` or `borrowed`; `BOOL` is `0` or
`1`; `SIGN` is `zero`, `positive` or `negative`. These names encode the enum
values in the headers, not extra strings. `P` and `H` are `p` and `h` followed
by the row index in `prims.def` and `hostops.def`. Each opcode's operand
kinds and counts are those in `opcodes.def`; field and case operands use `U`.
Jump targets use numeric word offsets. The assembler derives instruction
heads, including variable operand counts, without changing the code words.

`Q` is a double-quoted byte string. It accepts printable ASCII other than
`"` and `\`, raw UTF-8, and only the escapes `\"`, `\\` and `\xhh` (exactly
two hex digits for one byte). Raw control bytes are not allowed; the decoded
string must be valid UTF-8. `HEX` is `hex"` followed by an even number of
hex digits and `"`, with no spaces inside; each pair is one byte in file
order. `DIGITS` and `HEXDIGITS` are one or more decimal and hex digits,
respectively. The constant's type selects exactly one payload form:

| Type | Payload | Canonical spelling |
| --- | --- | --- |
| `unit` | `()` | `()` |
| `bool` | `true` or `false` | the word for its value |
| `byte` | unsigned decimal integer | `0` to `255`, without leading zeros |
| `char` | `U+` and its Unicode scalar value in hex | `U+0`, `U+41`, `U+1f600`; lowercase hex without leading zeros |
| `int` | signed decimal integer | `0`, `7`, `-7`; no leading zeros, `+` or negative zero |
| `time` | signed decimal integer, microseconds since the epoch of 2.2 | as `int`, not a date string |
| `duration` | signed decimal integer, microseconds | as `int`, without a unit suffix |
| `decimal` | exact decimal number | `0`, `7`, `-0.125`; the rules below |
| `float` | `bits HEX`, exactly 8 bytes | lowercase hex in file byte order |
| `text` | `text S` | the string index |
| `bytes` | `bytes HEX` | lowercase hex in file byte order |
| `bigint` | `bigint SIGN HEX` | canonical sign and little-endian magnitude (3.1) |
| `function` | `closure F` | the capture-less closure's function index |

Integer payloads have no fraction; unsigned ones have no sign. `NUMBER`
accepts no `+` sign, exponent, separators or type suffix. A decimal
has an integer part and at most 18 fractional digits. Its numeric value
times 10¹⁸ is the signed coefficient in 3.1; parsing never rounds or goes
through `float`.
Canonical decimals have no integer leading zeros except `0`; a fraction is
printed only when nonzero, without trailing zeros. Fractional leading zeros
are significant and remain. A minus sign is printed only for a negative,
nonzero value, so decimal zero is `0`. The assembler accepts integer leading
zeros, fractional trailing zeros and signed negative zero as noncanonical
spellings, within these rules. Every value must satisfy 3.1's type bounds.

`bits` is valid only for `float`: it retains negative zero and every accepted
NaN payload. No other scalar accepts raw bits, and a float accepts no natural
number spelling in indexed text. All other scalar encodings are uniquely
determined by their exact value. A mismatched payload is an error. Byte
strings may contain any bytes.

Registers list **all** register types, including parameters and a closure's
implicit `env`. Parameters list only explicit arguments. Module type exports
pair the type with `contents_exposed`: `0` for `T`, `1` for `T(..)`.
Module globals list all globals, not just exposed names. Debug entry `dN`
belongs to function `fN`; every function has one, including empty metadata.
Filename rows print canonically as lowercase `hex"..."`, including UTF-8 paths;
the assembler also accepts `Q` when its decoded bytes are valid UTF-8.
Span tuples are code offset, optional filename index, start byte and end byte.
`(@N, -, 0, 0)` clears source attribution from word N until the next span
entry or code end; it is retained even beside another absent/equal span.
Variable tuples are register, name string, inclusive start word and exclusive
end word.
Their list order is preserved, not sorted by register. Debug `text` preserves
the test sentence or spec line, including the distinction between absent
and an empty string. `declaration` gives optional file/start/end;
`subject` links a promise to its ordinary function or an anchor to its promise.
`expectation` encodes `NOT_ANCHOR = 0` as `none`, `ALLOWS = 1` as `allows`,
and `FORBIDS = 2` as `forbids`, not a string reference. `lines` contains
optional file/start/end/text tuples for promise conditions, in source order;
duplicate text indices and absent locations are preserved. An absent file
requires start/end zero in every location, code span and condition tuple.
These fields are always explicit, including `declaration (-, 0, 0)`,
`subject -`, `expectation none` and `lines []`. Section 3.4's kind/link rules
apply; nothing is inferred from a display name or condition text.
The header's magic, version and zero flags are fixed;
the assembler regenerates its checksum. A verified header has no other data
to preserve.

### 10.2 Shorthand

Shorthand uses Mo type syntax, symbolic instruction operands and literals.
It may start with **pool seeds**: indexed strings, types, layouts, constants
and files sections, in that order, each optional. Seeds are complete prefixes
with consecutive indices; they may refer forward to later seeds or named
declarations through their eventual indices. They are retained unchanged.
No indexed globals, init, functions, modules or debug sections occur in
shorthand, and no shorthand items occur in indexed text.

```text
shorthand   = "bytecode 0.2 shorthand"
              [ strings ] [ types ] [ layouts ] [ constants ] [ files ] { item }
item        = nominal | global_decl | init_decl | function_decl | module_decl
nominal     = "record" NAME { named_field } "end"
            | "choice" NAME { "case" NAME [ "(" named_fields ")" ] } "end"
named_field = NAME ":" TYPE
named_fields = TYPE | named_field { "," named_field }
global_decl = "global" NAME ":" TYPE "in" NAME
init_decl   = "init" list(NAME)
module_decl = "module" NAME "exposes" list(exposure)
exposure    = NAME [ "(..)" ]
function_decl = "function" NAME "(" [ params ] ")" ":" TYPE KIND [ "spec" ]
              "in" NAME [ "captures" list(TYPE) ]
              "registers" list(named_reg)
              [ "variables" list(named_variable) ] [ "text" Q ]
              [ "declaration" named_location ] [ "subject" named_subject ]
              [ "expectation" EXPECTATION ] [ "lines" list(named_spec_line) ]
              { label | source_span | named_instruction } "end"
params      = NAME ":" TYPE OWNERSHIP { "," NAME ":" TYPE OWNERSHIP }
named_reg   = R ":" TYPE
named_variable = "(" R "," Q "," LABEL "," LABEL ")"
named_file  = Q | HEX | PATH | "-"
named_location = "(" named_file "," U "," U ")"
named_subject = NAME | F | "-"
named_spec_line = "(" named_file "," U "," U "," Q ")"
label       = LABEL ":"
source_span = "span" named_file U U
named_instruction = OPCODE [ named_operand { "," named_operand } ]
named_operand = R | LABEL | AT | NAME | TYPE | LITERAL | T | L | K | G | F | P | H | U
```

`NAME` is a Mo qualified name, including type arguments for instantiations;
`TYPE` is a Mo type or an indexed `T`, with `env(NAME)` for a closure's env.
In symbolic names, spaces and tabs around type-argument punctuation are
ignored, a comma is stored as comma-space and a colon as space-colon-space:
`Wrapper(Int,Text)` and `Wrapper(Int, Text)` name the same declaration,
as do `Box(fn(Int):Int)` and `Box(fn(Int) : Int)`. Other name bytes, including
case, are unchanged. Seeds retain their bytes; a symbolic lookup of a seeded
generic name uses this same normalized spelling. An index-shaped name (an
index prefix followed entirely by digits) cannot be a symbolic type, function,
global or module declaration; use indexed text to retain such names. Local
field, case and parameter names are not references and may be index-shaped.
Names such as `t1x` are symbolic names, not indices.
A label is `@` followed by a name; a final label may mark the code's end for
a variable range, but not a jump target. `registers` lists only additional
registers, consecutively after explicit parameters and a closure's implicit
owned `env`; the assembler supplies those leading register types. Parameter
names do not implicitly create debug ranges. `variables` gives those ranges;
`span` adds an entry at the next instruction's word offset. Equal adjacent
spans are not merged. Only one span may occur at an offset.
`span - 0 0` explicitly clears attribution. An absent file requires zero
start/end here and in declaration/condition locations.
`text` supplies a test sentence or spec line. Omitted captures, variables and
spans are empty; omitted text is absent. Omitted `declaration` is `(-, 0, 0)`,
omitted `subject` is absent, omitted `expectation` is `none`, and omitted
`lines` is empty. A supplied subject resolves to a function by full name or
index, including forward references; it does not intern a name string.
Conditions retain list order and text, not parsed predicates. Nothing infers
links, expectations or conditions from function names, kind or spec text.
Omitted `spec` means `0`, not
inference from the kind; section 9 still checks it. For ordinary functions,
debug text must be absent.

Functions, globals, primitives and host operations are written by full name,
and layouts by their type (`Type.Case` for a case block, `closure(NAME)` for
a closure). Index-shaped tokens are reserved for indexed references. An
indexed operand refers directly to a final table entry and does not create
it; numeric `AT` targets are also allowed. A constant operand may be a Mo
scalar, text, bytes or bigint literal, or `closure(NAME)` for a capture-less
closure. Its type is the destination register's type, without
conversion. Numeric literals are encoded as in 3.1; a float literal uses its
binary64 value, `nan` the bits `0x7ff8000000000000`. Other NaN payloads need a
seed. A `range` or choice literal is not a constant (3.1).
For Byte, Int, Time and Duration, shorthand also accepts `0x` hexadecimal
and `0b` binary digits, and underscores between digits, with the same sign
and value bounds. Decimal keeps 10.1's exact decimal syntax. Float accepts
decimal digits with an optional fraction and optional `e`/`E` exponent;
an exponent may have `+` or `-`. Conversion uses the C numeric locale,
independently of the caller's locale, and retains signed zero. Overflow and
underflow produce the binary64 infinity or rounded value, respectively.
Char accepts 10.1's `U+hex` or one single-quoted Unicode scalar, with escapes
`\n`, `\r`, `\t`, `\0`, `\'`, `\"`, `\\` and `\u{hex}` (one to six
hex digits). Text uses `Q` with exactly 10.1's escapes; Bytes uses `bytes HEX`.
BigInt accepts a signed decimal integer or `bigint SIGN HEX`. Decimal BigInt
spellings have at most `MO_LIMIT_BYTECODE_BIGINT_DIGITS` (4,096) digits,
including leading zeros; they have no radix prefix or underscores. Conversion
uses nine-digit chunks and base-2^32 limbs, keeping its bounded quadratic work
small even in a text filled with maximum-length decimal literals.
Larger values use the linear hex spelling, up to section 3's magnitude limit.
Negative zero becomes zero. None of these shorthand spellings are printed
by the disassembler in place of 10.1's canonical payloads.

The assembler fills the tables in these steps:

1. Copy the seeds. Reserve a fresh nominal type for each record or choice
   declaration, then indices for globals, functions and modules, each in its
   own declaration order. Seeded nominal types are already declared: do not
   declare them again. Resolve names, including forward references. Type,
   function, global and module declarations have separate namespaces; a record
   and a module may both be `Account`. Duplicate names within one namespace are
   errors; use indexed text for files with equal nominal names. A name shared
   by a type and a function is ambiguous only in an exposure list, and is refused
   there. Every global and function names its module with
   `in`; modules are declared exactly once. No implicit module is added.
2. Walk items in text order, headers before bodies, lists and instruction
   operands left to right. Visit a type's parameters before interning the
   enclosing type. Named nominal references use their reserved indices, not
   a recursive walk of their fields or name's type arguments. A reference to
   `Column(Product)` neither interns nor requires a separate `Product` type.
   Intern other types on first encounter,
   reusing a matching seed or earlier entry. Function headers visit explicit
   parameter types, result, captures, implicit env, then additional registers.
   A closure also interns its function type after its result type. Define the
   reserved nominal entries from their fields and cases.
3. Record requests for symbolic layout operands in encounter order.
   `make_case` also requests its case layout if boxed; `make_closure` requests
   its closure layout. After all types are defined, process requests in that
   order: append the layout with its definition's fields or captures if
   absent, or reuse the lowest matching index. Nothing else adds layouts.
   Cache the index by layout kind, type and case/function: a distinct layout's
   fields or captures are read once, not once per operand. A layout request
   does not visit its definition's types again. Validate literals at their
   encounter (unless their indexed type is still forward), deferring only
   constant pooling until complete types are known. On a literal,
   append a constant if absent; equality is its type index and exact encoded
   payload, not runtime equality. Reuse the lowest matching constant index.
4. Intern strings at each encounter in the same walk: declaration names,
   then field/case names, function parameter names, variable names, spec text,
   condition text and text literals. Intern filename bytes separately at
   declaration, condition and code-span locations, reusing the lowest
   byte-identical filename row or appending after file seeds. `PATH` references
   do not insert rows; absence inserts nothing into either pool. Types supplied by seeds do not
   create strings on symbolic lookup. Reuse the lowest byte-identical string
   index; otherwise append. Module references in `in` do not create strings;
   neither do other symbolic lookups. A module's declaration creates its name.
   Unused parameter-name strings are retained. Names in type entries obey
   section 3's lowest-index rule.
5. Encode instructions and resolve labels. Build each module's exposed-type
   and exposed-function arrays from its exposure list, in list order; a bare
   type hides its contents, `(..)` exposes them and is invalid on a function.
   Parse the full instantiated name before peeling off a trailing `(..)`:
   `Page(Int)` and `A.id(Int)` are bare exposures, not parameter lists.
   A global in the list is an error: module globals include all declarations
   with that `in` module, in global declaration order. There is at most one
   `init` item; its list gives exactly the initializer order, or is empty when
   omitted. Emit one debug entry per function from its explicit metadata,
   preserving variable/condition list order and explicit clearing spans;
   resolve subjects without inventing links. Regenerate the header and verify the file.

Steps 2 to 4 describe one encounter walk, not three separate passes over the
text; layout requests wait for complete definitions, but keep that walk's
order. Only index reservation in step 1 precedes it. A seed prevents automatic
insertion of its matching entry and so can set order independently of use.
Without seeds, constants, non-nominal types and layouts follow first use.
Shorthand and seeds therefore describe the same file as indexed text, not a
second binary format.

### 10.3 A worked text

Both texts below assemble to the same file. `s0` is unused. The types
`List(Int)` and `(Int, Int)` occupy `t2` and `t3`, though the function first
mentions the tuple, then the list. Layout `L0` is for `Visible`, though the
code constructs the tuple with `L1` first. `Hidden` is exposed without its
contents; `Visible` with them. Variable `answer` lives in words [3, 21), and
the test's sentence is `keeps the answer`. Its source attribution is cleared
at word 3. `Demo.identity` returns its argument; its promise checks
`result == n`, then `n >= 0`, returning 1 or 2 for the first false line and
0 on success. The two anchors demonstrate explicit `allows` and `forbids`
links. Declarations and conditions include both present and absent locations.

This is the canonical indexed text:

```text
bytecode 0.2 indexed
strings
  s0 "unused"
  s1 "Hidden"
  s2 "Visible"
  s3 "value"
  s4 "Demo"
  s5 "Demo.test.1"
  s6 "answer"
  s7 "keeps the answer"
  s8 "demo.mo"
  s9 "Demo.identity"
  s10 "n"
  s11 "Demo.identity.promise.1"
  s12 "result"
  s13 "promise \"identity\""
  s14 "result == n"
  s15 "n >= 0"
  s16 "Demo.identity.promise.1.allows.1"
  s17 "allows n: 0, result: 0"
  s18 "Demo.identity.promise.1.forbids.1"
  s19 "forbids n: 1, result: 0"
end
types
  t0 unit
  t1 int
  t2 list t1
  t3 tuple [t1, t1]
  t4 record s1 [(s3, t1)]
  t5 record s2 [(s3, t1)]
  t6 bool
end
layouts
  L0 record t5 [t1]
  L1 tuple t3 [t1, t1]
end
constants
  k0 t1 7
  k1 t0 ()
  k2 t1 0
  k3 t1 1
  k4 t1 2
end
globals
end
init
end
functions
  f0 s5 m0 test spec 1
    params []
    result t0
    captures []
    registers [t1, t3, t2, t5, t0]
    code
      const r0, k0
      construct r1, L1, r0, r0
      drop r1
      construct r3, L0, r0
      drop r3
      const r4, k1
      ret r4
    end
  end
  f1 s9 m0 function spec 0
    params [(t1, owned)]
    result t1
    captures []
    registers [t1]
    code
      ret r0
    end
  end
  f2 s11 m0 promise spec 1
    params [(t1, owned), (t1, owned)]
    result t1
    captures []
    registers [t1, t1, t6, t1]
    code
      eq_int r2, r1, r0
      branch r2, @8, @24
      const r3, k2
      le_int r2, r3, r0
      branch r2, @19, @29
      const r3, k2
      ret r3
      const r3, k3
      ret r3
      const r3, k4
      ret r3
    end
  end
  f3 s16 m0 anchor spec 1
    params []
    result t1
    captures []
    registers [t1, t1]
    code
      const r0, k2
      call r1, f2, r0, r0
      ret r1
    end
  end
  f4 s18 m0 anchor spec 1
    params []
    result t1
    captures []
    registers [t1, t1, t1]
    code
      const r0, k3
      const r1, k2
      call r2, f2, r0, r1
      ret r2
    end
  end
end
modules
  m0 s4 types [(t4, 0), (t5, 1)] functions [f1] globals []
end
files
  P0 hex"64656d6f2e6d6f"
end
debug
  d0 spans [(@0, P0, 10, 20), (@3, -, 0, 0)] variables [(r0, s6, @3, @21)] text s7 declaration (P0, 0, 21) subject - expectation none lines []
  d1 spans [] variables [] text - declaration (P0, 22, 39) subject - expectation none lines []
  d2 spans [] variables [] text s13 declaration (P0, 40, 90) subject f1 expectation none lines [(P0, 50, 60, s14), (-, 0, 0, s15)]
  d3 spans [] variables [] text s17 declaration (P0, 91, 110) subject f2 expectation allows lines []
  d4 spans [] variables [] text s19 declaration (-, 0, 0) subject f2 expectation forbids lines []
end
```

The shorthand seeds the unused string, types and layouts. The assembler adds
the remaining strings and constants on encounter; the seeds need not include
them. The unused `r2` keeps a list type in the register table, not a value.
In `Demo.identity`, `registers []` adds no registers beyond parameter `r0`.
The shorthand spells out metadata defaults here to show their indexed form.

```text
bytecode 0.2 shorthand
strings
  s0 "unused"
  s1 "Hidden"
  s2 "Visible"
  s3 "value"
  s4 "Demo"
  s5 "Demo.test.1"
  s6 "answer"
  s7 "keeps the answer"
  s8 "demo.mo"
end
types
  t0 unit
  t1 int
  t2 list t1
  t3 tuple [t1, t1]
  t4 record s1 [(s3, t1)]
  t5 record s2 [(s3, t1)]
  t6 bool
end
layouts
  L0 record t5 [t1]
  L1 tuple t3 [t1, t1]
end
module Demo exposes [Hidden, Visible(..), Demo.identity]
function Demo.test.1() : Unit test spec in Demo
  registers [r0: Int, r1: (Int, Int), r2: List(Int), r3: Visible, r4: Unit]
  variables [(r0, "answer", @live, @done)]
  text "keeps the answer"
  declaration ("demo.mo", 0, 21)
  subject -
  expectation none
  lines []
  span "demo.mo" 10 20
  const r0, 7
  @live:
  span - 0 0
  construct r1, (Int, Int), r0, r0
  drop r1
  construct r3, Visible, r0
  drop r3
  const r4, ()
  ret r4
  @done:
end
function Demo.identity(n: Int owned) : Int function in Demo
  registers []
  declaration ("demo.mo", 22, 39)
  subject -
  expectation none
  lines []
  ret r0
end
function Demo.identity.promise.1(n: Int owned, result: Int owned) : Int promise spec in Demo
  registers [r2: Bool, r3: Int]
  text "promise \"identity\""
  declaration ("demo.mo", 40, 90)
  subject Demo.identity
  expectation none
  lines [("demo.mo", 50, 60, "result == n"), (-, 0, 0, "n >= 0")]
  eq_int r2, r1, r0
  branch r2, @first_passed, @first_false
  @first_passed:
  const r3, 0
  le_int r2, r3, r0
  branch r2, @success, @second_false
  @success:
  const r3, 0
  ret r3
  @first_false:
  const r3, 1
  ret r3
  @second_false:
  const r3, 2
  ret r3
end
function Demo.identity.promise.1.allows.1() : Int anchor spec in Demo
  registers [r0: Int, r1: Int]
  text "allows n: 0, result: 0"
  declaration ("demo.mo", 91, 110)
  subject Demo.identity.promise.1
  expectation allows
  lines []
  const r0, 0
  call r1, Demo.identity.promise.1, r0, r0
  ret r1
end
function Demo.identity.promise.1.forbids.1() : Int anchor spec in Demo
  registers [r0: Int, r1: Int, r2: Int]
  text "forbids n: 1, result: 0"
  declaration (-, 0, 0)
  subject Demo.identity.promise.1
  expectation forbids
  lines []
  const r0, 1
  const r1, 0
  call r2, Demo.identity.promise.1, r0, r1
  ret r2
end
```

### 10.4 Canonical text and round trips

Canonical text is indexed text with these printing rules:

- Sections and entries follow file order, without comments or blank lines.
  Section names and their `end` are unindented. Entries have two spaces;
  function properties and their `end` have four; instructions have six.
  A function has separate `end` lines for code and for the entry, as above.
- Each grammar entry occupies one line, except a function's prescribed lines.
  Tokens have one space between them except beside punctuation. Every comma
  is followed by one space and preceded by none. Tuples have no space after
  `(` or before `)`, nor lists just inside brackets. There are no trailing
  spaces. Each function property (`params` through `code`) has its own line.
- Indices and offsets use decimal without leading zeros, except `0`;
  scalar payloads follow 10.1. Hex digits are lowercase. Quoted strings
  print ASCII bytes 0x20 to 0x7e literally except `"` and `\`, which use
  `\"` and `\\`. Every other byte uses `\xhh`,
  including each byte of non-ASCII UTF-8. No Unicode normalization occurs.
- Lines end with LF, including the last line. No terminating NUL is printed.
  Every array and spec flag is explicit; empty arrays are `[]` and absent
  indices are `-`. There are no aliases, symbolic operands, labels or
  first-use insertions.
- Each debug entry is one line, with `spans`, `variables`, `text`,
  `declaration`, `subject`, `expectation` and `lines` in that order. Locations
  always print all three fields, even when absent. Expectation uses exactly
  `none`, `allows` or `forbids`; clearing spans and condition order are never
  normalized away.

The assembler also accepts comments, blank lines, spaces or tabs at token
boundaries, CRLF, a missing final newline, leading zeros in decimal numbers
and uppercase hex digits. These do not make text canonical. Indexed sections,
entries and property lines must still occur in grammar order. All references
and metadata are checked by section 9, just as for a binary file.

For every verified file `b`, `assemble(disassemble(b))` is `b` byte for byte.
For every accepted text `x`, `disassemble(assemble(x))` is its canonical
indexed text. Disassembling that file again prints exactly the same text.
These are format rules, subject only to available memory and the caller's
output buffer limit, not to first-use ordering. The bytecode text tests check both rules,
including unused and duplicate strings, non-first-use types and layouts,
float bits, exposure flags, spans (including clearing entries), variable ranges,
spec text, declaration locations, subject links, all three expectations and
ordered condition lines with present/absent locations. A fuzz
target checks the byte round trip.

The assembler and disassembler live in `src/bytecode`.
`mo_bytecode_assemble(text, length, out, error_out)` writes verified bytecode,
bounded by `MO_LIMIT_BYTECODE_SIZE`.
`mo_bytecode_disassemble(bytes, length, out, error_out)` verifies its input
before writing canonical indexed text. Text input and output are bounded by
`MO_LIMIT_BYTECODE_TEXT_SIZE` (2 GiB in v0.2), eight times the binary limit:
quoting and instruction names expand bytes. The canonical spelling above
fits within eight times the binary file's length, so every verified file's
text fits and text byte offsets still fit `u32`. Noncanonical input past the
text bound is refused. The expanded debug spelling preserves this bound:
even allowing ten decimal digits per u32, its fixed entry line with empty
arrays is at most 160 text bytes for 36 binary bytes. Each code span, variable
range or condition line adds at most 54 text bytes (including a list separator)
for 16 binary bytes. The newly added fixed fields alone add at most 104 text
bytes for 24 binary bytes, so extending the earlier spelling cannot consume
more than eight times the added binary bytes. Absent references are shorter;
condition text remains a string index, with its bytes quoted only in the
string section. Its at-most-fourfold byte escaping is unchanged.
Their declarations are in
`bytecode.h`. Each borrows its
input and requires an initialized, empty caller-owned buffer, which must not
back the input. Invalid input, an output limit or no memory returns false,
leaves the buffer empty (capacity may have grown), and fills the required
error structure with static reason text. Assembly errors have a text byte
offset; disassembly uses `mo_load_error` with an input byte offset. Offset zero
denotes a failure not tied to an input byte, such as no memory or an output
limit. Syntax refusals point to the offending token or byte, or text end when
a token is missing. Final-pool index refusals point to the reference token,
including forward references. Loader/verifier refusals map the offending
binary field or instruction back to its source token. Relational refusals
whose binary offset is the payload start carry an internal section/entry site
and point to that entry's indexed line or shorthand declaration/operand, not
the first string in the file. A section-count refusal points to its section
keyword (or an unpaired shorthand declaration). Their reason retains the
loader's violated rule; this extra site does not change public binary offsets.
Assembly uses wire-sized ceilings for encoded bytes and position tables,
an arena budget derived from text length through `mo_bytecode_work_budget`,
and `MO_LIMIT_NESTING_DEPTH` for nested types, names and literals. Incremental
symbol/pool lookups use balanced ordered trees, not linear scans or unkeyed
hashing; module-global arrays are built by buckets, and choice case offsets
are indexed once. Apart from bounded numeric conversion and nesting, work is
near-linear in text bytes plus generated wire bytes, with logarithmic lookup
factors. Verification keeps section 9's own budgets.
First-use table construction is only for shorthand; it never
renumbers a seed or an indexed file.

## 11. Versions

The format's version is 0.2. In Phase 2 the same build writes and reads a file,
so the loader accepts only its own version. Phase 5's upgrades need files
built by one version to load in another, and decide then what stays
compatible.
