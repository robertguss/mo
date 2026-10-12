# Mo Bytecode v0.1

**Draft, 12 October 2026, revised after a second review.** This is the
contract between Mo's compiler and its virtual machine, part of Architecture
v0.1 (`architecture.md`). It fixes the file format, the instruction set, the
representation of values, the calling convention and the rules the verifier
enforces. In Phase 2, stream 2's compiler writes this format through
`src/bytecode`'s writer, and stream 3 owns `src/bytecode` and the VM that
reads, verifies and runs it.

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
| `capability` | which: `Args`, `Env`, `Console`, `Files`, `ReadFiles`, `Clock` or `Random` | reference |

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
| `ref` | one field, of a reference type | the field's pointer |
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

The debug heap asserts all three (Architecture section 11).

## 3. The file

All integers are little-endian. A file is a header, then nine sections in this
order, each a 32-bit count followed by its entries.

```text
header      magic "MOBC", format version (major and minor, 16 bits each),
            flags (32 bits, zero in v0.1), checksum (FNV-1a, 64 bits, of
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
debug       spans, file names and register names (3.4)
```

The checksum detects truncation and corruption. It's no defense against an
attacker: Phase 5 signs upgrade packages separately. Every count and index has
a limit in `src/base/limits.h`, and the loader checks every one before
using it.

### 3.1 Constants

Each constant has a type and a value:

- a `unit`, `bool`, `byte`, `char`, `int`, `float`, `time` or `duration` in 8
  bytes, and a `decimal` in 16;
- a `text` as a string index;
- `bytes` as a length and the bytes;
- a `bigint` as a sign and its magnitude's bytes, with no leading zeros;
- or a capture-less closure, as a function index.

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

### 3.4 Debug information

For every function there is:

- a table from code offsets to spans (file, start byte and end byte);
- for each register that holds a source variable, its name and the code range
  where it holds it, since a register can hold different variables at
  different times;
- for a `test`, its sentence, and for an `example`, `promise` or `anchor`, the
  text of its line.

File names are strings. The VM needs this section to report bugs, and Phase 4
needs it to show a running program.

### 3.5 Names

A function's name is its module and its source name, as `Wc.count`. An
instantiation of a generic function adds its type arguments, as
`List.map(Int, Text)`. Other kinds are named after what they come from, with a
number counted from 1 in source order: `Wc.test.2`, `Wc.count.example.3`,
`Account.transfer.promise.1`, `Account.transfer.promise.1.allows.2`,
`Account.transfer.promise.1.forbids.1`, `Kv.max_ttl.init`, and
`Wc.count.fn.1` for the first anonymous function in `Wc.count`.

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
- A register an instruction consumes can't appear in that instruction twice.
- An instruction reads and consumes its operands first, then writes its
  result. The result register must not be `owned` or hold a token at that
  point. So `prim r1, List.append, List(Int), r1, r2` is valid: it consumes
  `r1` and then writes it. But `dup r0, r0` on an owned `r0` isn't, because it
  would overwrite an owned reference.
- The result becomes `owned` for a reference type and `value` for an immediate
  type, unless the instruction says otherwise.

For a parameter of an immediate type, `owned` and `borrowed` mean nothing.

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
| `const r_d, k` | loads a constant; a reference is immortal, so `r_d` is borrowed |
| `global r_d, g` | loads a module constant; `r_d` is borrowed |
| `set_global g, r_s` | only in `init` functions; consumes `r_s` and makes it immortal, with everything it reaches |

Freeing never recurses on the C stack. Dropping a long chain of nested values
uses an explicit work list, and so do equality, hashing, comparison and
printing (section 8).

### 4.4 Arithmetic, comparison and ranges

These work on immediates, so ownership doesn't apply. Each family has one
opcode per type it accepts:

| Family | Types | Bugs |
| --- | --- | --- |
| `add`, `sub`, `mul` | `int`, `decimal` | overflow |
| `add`, `sub`, `mul` | `float` | none: IEEE 754 |
| `div`, `rem` | `int`, `decimal` | division by zero; overflow (`int` minimum ÷ −1, and its remainder) |
| `div`, `rem` | `float` | none |
| `neg` | `int`, `decimal`, `float`, `duration` | overflow (the minimum) |
| `add`, `sub` | `duration` | overflow |
| `mul_int` | `duration` by `int` | overflow |
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
is a primitive (section 6).

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
| `prim r_d, p, t, r_1, …, r_n` | a primitive (section 6). `t` is the type it is instantiated at, such as the list type for `List.get`, so the VM knows which elements are references |
| `host r_d, h, r_c, r_1, …, r_n` | a capability operation (section 7). `r_c` is the capability, which it reads |

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

## 5. Calls and frames

### 5.1 Frames

Frames live on the VM's own stack, not the C stack. A call pushes a frame of
the callee's register count and copies the arguments into registers 0 to n − 1.
`ret` copies the result into the caller's `r_d` and pops the frame. The stack
has a size limit and the call depth a count limit. Exceeding either is the bug
"out of stack" (reference 13.2), with a report.

### 5.2 Tail calls

A tail call replaces the current frame. The arguments are copied to a scratch
area first, since they come from the frame being replaced. If the callee
needs more registers, the frame grows in place, since it is at the top of the
stack. So a tail call never grows the stack beyond the larger of the two
frames, and loops written as recursion run in constant space (reference 5.3).
Every call in tail position is compiled as a tail call, including calls to
closures.

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
  declared there. So `lib/text.mo` holds only the functions written in Mo, and
  nobody writes a primitive's signature twice.
- **Lowering** turns a call to a primitive into a Core primitive operation.
- **The code generator** emits `prim` with the instantiation type, and treats
  each parameter as owned (`o`) or borrowed (`b`).
- **The verifier** checks the argument and result types against the signature
  at that type.
- **The VM** dispatches on the id to a C function in `src/prim/`.

A primitive that "changes" a collection, such as `append`, `set`, `remove` or
`take`, consumes it and follows section 4.6's two paths. A primitive that only
reads, such as `get`, `size` or `contains?`, borrows. The list in `prims.def`
is written first in Phase 2, from reference section 14 and these rules, and
grows only with the library.

## 7. Capabilities and host operations

A capability is a heap object that carries its own operations. It has:

- which capability it is;
- a pointer to its operations table, real or fake;
- for a fake, an owned reference to the fake's **state**;
- for `Files` and `ReadFiles`, its folder: an open handle for a real one, and
  a path prefix for a fake one;
- and whether it is read-only.

`host r_d, h, r_c, …` calls operation `h` of `r_c`'s table.
`src/base/hostops.def` lists the operations, with the same columns as
`prims.def`:

- `Console`: `print`, `error`, `read_line`, `printed` and `printed_errors`;
- `Files`: `read_text`, `read_bytes`, `write_text`, `append_text`, `remove`,
  `list`, `exists?`, `within` and `read_only`;
- `Clock`: `now`. `Random`: `int`. `Args`: `list`. `Env`: `get`;
- the fake constructors: `Console.fake`, `Console.fake_input`,
  `Files.in_memory`, `Clock.fixed`, `Random.seeded`, `Args.of` and `Env.of`.

The verifier allows the fake constructors, `printed` and `printed_errors` only
in spec code (3.3), which is where the reference allows them (reference
12.4). Code that isn't
spec code can't call, tail-call or make a closure of a spec function or an
`init` function (section 9, item 6). So `main`, and everything it reaches, can
neither make a fake nor run a test's code, even in an image that contains
tests.

**A fake's state** is the one mutable object in the heap, and only its slots
change. What they hold are ordinary counted values: texts, bytes, and lists
and maps of them, never a capability or a closure, so the heap stays acyclic.
They change only the way programs change values: `Files.in_memory(files)`
consumes its map, so if the test still holds the map, the first write copies
it; `printed` returns a duplicate of the state's list, so printing more later
doesn't change a list the test holds. Every handle on one fake shares its
state: the `Console.fake()` passed to `main` and the one the test reads
`printed` from are the same object. `files.within("a")` and `files.read_only`
make new handles on the same state, with a longer prefix or read-only set.

**A real capability** is made by the runtime, once, for `main`. Dropping a real
`Files` or `ReadFiles` handle closes its folder's descriptor. The number of
open handles has a named limit, and `within` past it is a bug.

## 8. Generic operations in the VM

The VM implements a few operations for every type, driven by the type table:

- **Equality and hashing** of any type with `Equal` or `Hash`, as `equal` and
  inside the map and set primitives. They are always structural (reference
  9.2), so they never run Mo code. Maps and sets compare their entries in any
  order, and hash their entries in a way that doesn't depend on order.
- **Dropping** any value (4.3).
- **The structural text form** of any value, for defect reports and Phase 4's
  introspection. It follows reference 9.2 without anyone's own `to_text`, so
  printing a value can never stop: names from the type table; `Some(3)` for a
  case's unnamed field; sets as `[1, 2].to_set`; floats in their shortest
  round-trip spelling (Ryu), with not-a-number as `nan` and the infinities as
  `inf` and `-inf`; decimals in their shortest exact spelling; times
  in RFC 3339; durations in seconds, as `1.5s`; texts and characters quoted and
  escaped inside other values.

All of them use explicit work lists, never C recursion, so a deeply nested
value can't overflow the C stack. Order and the text forms that programs see,
including customized `compare` and `to_text`, are Mo functions generated by
lowering for each type (Architecture section 4.3), not VM operations.

Map and set hashing uses SipHash-1-3 with a key chosen when the VM starts, as
Rust and Python do, so a program fed hostile keys can't be slowed by
collisions. Maps and sets keep insertion order, so the key never changes what
a program computes. (`mo test`'s generator has its own hash, with a fixed
key: Architecture section 10.1.)

## 9. Verification

The loader refuses a file unless the verifier accepts all of it. The verifier
checks:

1. **Structure.** The header, version and checksum; every count and index
   within its limit and its table; every string valid UTF-8; every constant
   valid (3.1).
2. **Types and layouts.** Every type well-formed (2.1); every layout's fields
   matching its record, tuple, case or closure; every choice type with at
   least one case and at most 65,535.
3. **Code shape.** Every function's code decodes into whole instructions, with
   the right number of operands for each opcode; every jump target is the
   start of an instruction in the same function; every path ends as section
   4.5 requires.
4. **Types of operands.** Each instruction's registers have the types its
   opcode, primitive or host operation requires at its instantiation type;
   calls match their targets' parameter and result types; `ret` matches the
   function's result type; `make_closure` matches its function; `switch` has
   one target per case.
5. **Ownership and definition.** A dataflow over the states of section 4.2,
   with known cases, per function. Every read and every consume follows 4.2.
   At every join, each register's state is the same on every incoming edge.
   `ret` and tail calls leave nothing owned except what they consume. `stop`
   and `fail` may leave owned registers: they end the process, whose heap is
   freed all at once (Architecture section 8).
6. **Kinds of function.** `set_global` only in `init` functions, and every
   global set by exactly one of them, listed once in the `init` section.
   `fail`, the fake constructors, and `Console`'s `printed` and
   `printed_errors` only in spec code (reference 12.4). Code that isn't spec
   code doesn't call, tail-call or make a closure of a spec function or an
   `init` function. No function of a kind other than `function` has a
   borrowed parameter. Each kind has its signature (3.3).

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

## 10. The text form

Every file has a text form. It is used to write the VM's tests by hand, and
`mo check --emit=bytecode` prints it. It is line-based, with `;` starting a
comment:

```text
file        = { item }
item        = type | global | init | function | module
type        = "record" NAME { field } "end"
            | "choice" NAME { "case" NAME [ "(" fields ")" ] } "end"
field       = NAME ":" TYPE
global      = "global" NAME ":" TYPE
init        = "init" NAME { "," NAME }                 ; the init functions in order
module      = "module" NAME "exposes" NAME { "," NAME }
function    = "function" NAME "(" [ params ] ")" ":" TYPE KIND [ "spec" ]
              [ "captures" TYPE { "," TYPE } ]
              "registers" REG ":" TYPE { "," REG ":" TYPE }
              { [ LABEL ":" ] [ "span" FILE START END ] instruction }
              "end"
params      = NAME ":" TYPE ( "owned" | "borrowed" ) { "," ... }
instruction = OPCODE operand { "," operand }
operand     = REG | LABEL | NAME | TYPE | LITERAL
```

Types are written in Mo's syntax. Functions, globals, primitives and host
operations are written by name, and layouts by their type's name (a case
block as `Type.Case`). Constants are written as Mo literals where they are
used, such as `const r1, "total"`; the assembler builds the constant pool,
in order of first use. Structural types and layouts are added to their tables
in order of first use, too, which makes the bytes depend only on the text.

```text
function Wc.add(a: Counts borrowed, b: Counts borrowed) : Counts function
  registers r2: Int, r3: Int, r4: Int, r5: Int, r6: Int, r7: Int, r8: Int, r9: Int, r10: Int, r11: Counts
  field r2, r0, 0
  field r3, r1, 0
  add_int r4, r2, r3
  field r5, r0, 1
  field r6, r1, 1
  add_int r7, r5, r6
  field r8, r0, 2
  field r9, r1, 2
  add_int r10, r8, r9
  construct r11, Counts, r4, r7, r10
  ret r11
end
```

The assembler and disassembler live in `src/bytecode`. Assembling a text,
disassembling the bytes and assembling the result gives the same bytes. A fuzz
target checks this.

## 11. Versions

The format's version is 0.1. In Phase 2 the same build writes and reads a file,
so the loader accepts only its own version. Phase 5's upgrades need files
built by one version to load in another, and decide then what stays
compatible.
