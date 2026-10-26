# CBMC proofs

`make cbmc` (or `bash cbmc/prove.sh [proof...]`) proves parts of Mo with
[CBMC](https://www.cprover.org/cbmc/): the bytecode reader, `src/bytecode/read.c` (`reader.c`,
below), the arena, `src/base/arena.c` (`arena.c`, in [The arena](#the-arena)), the bytecode
verifier's transfer functions in `src/bytecode/verify.c` and `verify-instruction.c` (`verify.c`,
in [The verifier](#the-verifier)), the primitives' arithmetic and UTF-8 in `src/base/decimal.c`,
`src/prim/number.c`, `prim.c`, `text.c` and `unicode.c` (in [The primitives](#the-primitives)),
lexical path containment and bounded resolution (`path.c`, `resolve.c` and `resolve-kernel.c`, in
[The host paths](#the-host-paths)), and reference count updates (`refcount.c`, in
[Reference count updates](#reference-count-updates)), plus hash-table probing (`table.c`, in
[Hash-table probing](#hash-table-probing)).

For the bytecode reader, for **every input of up to
`MO_CBMC_INPUT` bytes** (32 by default, and more for the proofs whose loops need it), decoding:

- reads and writes only inside the objects it was given or allocated (bounds and pointer checks);
- never overflows a signed integer, shifts out of range, divides by zero, or loses a value in an
  implicit conversion;
- never fails an `MO_ASSERT`;
- and, when it refuses, leaves an error with a reason and an offset inside the input.

This is a bounded proof, not a test: CBMC checks every input of that size, not a sample. It
complements `fuzz/fuzz_bytecode_io.c`, which runs much larger inputs but only some of them.
Architecture section 11 ("Small critical C functions") and task V4 (MO-54) ask for it.

## How the proof is split

Proving the whole decoder in one run does not scale. So `reader.c` writes a specification for
each function of `read.c`: what it requires of the reader, and what it guarantees about the
reader, the input and the tables it returns. Each specification is used twice:

- **Proof.** `harness_<f>` builds any reader that meets `f`'s requirements, calls the real `f`,
  and asserts its guarantees, including that it changed nothing else.
- **Model.** `model_<f>` asserts `f`'s requirements, then forgets what `f` may change and assumes
  its guarantees. A table its callers store but never read comes back as a pointer to no object,
  so a caller that did read it would fail its proof. `prove.sh` swaps the model in for every call to `f` in the proofs of its callers
  (`goto-instrument --replace-calls f:model_f`).

So each proof covers one function, with its callees trusted only as far as their own proofs
show. `read.c` is included, not linked, so its static functions can be proved one by one. Its
loops over entries call one function per entry (`type_entry`, `choice_cases`, `parameters`,
`layout_entry`, `module_entry`, `file_entry`, `debug_entry`, `service_entry`), which keeps every
proof small.

| Proof | Checks | Replaces |
| --- | --- | --- |
| `word`, `boolean` | one little-endian word, offset kept inside the input | `word` |
| `allocate`, `scratch` | a fresh block of exactly the size asked, or a no-memory refusal | |
| `array`, `indices` | a count within its limit and the bytes left, and its table | `word`, `allocate`, `array` |
| `utf8` | `mo_bytecode_utf8` reads only its bytes | |
| `blob` | a length within the bytes left, copied and NUL-terminated | `word`, `allocate`, `mo_bytecode_utf8` |
| `name`, `fields`, `choice_cases` | name indices checked against the strings table | `word`, `array`, `name`, `fields` |
| `type_entry`, `types` | one type of each kind, then the types table | the helpers above |
| `constants`, `parameters`, `functions` | each constant and function, and their tables | the helpers above |
| `layout_entry`, `module_entry`, `file_entry`, `debug_entry` | one layout, module, filename or debug record | `word`, `array`, `boolean`, `indices`, `blob` |
| `service_entry` | one descriptor, including its tool and timer tables | `word`, `array`, `boolean`, `indices` |
| `sections` | every section in order, the reader's objects unchanged | every function above |
| `decode` | `mo_bytecode_decode_info`: header, checksum, trailing bytes, types' references | `allocate`, `sections` |

Loops are unwound to bounds the input size implies (a table of entries of at least 4 bytes has
at most `MO_CBMC_INPUT / 4` of them), and every bound carries an unwinding assertion, so a bound
that is too small fails the proof rather than hiding paths.

A proof can also hold because part of its function never runs: an input too small for a loop's
body, or an assumption that rules out the paths that matter. Two checks catch that:

- **Input sizes.** Each proof runs on inputs large enough for each of its loops to run once:
  `functions` needs 40 bytes (a function entry takes 36), `debug_entry` 52, `service_entry` 56
  and `sections` 64.
  `prove.sh` refuses a proof whose input leaves a loop bound below 2.
- **Reach.** `bash cbmc/prove.sh --reach` runs CBMC's location coverage on each proof and fails
  unless every block of the proved function runs on some input, and so does the end of its
  harness. The only blocks allowed to stay unreached are the failure branches of `MO_ASSERT`
  and `MO_UNREACHABLE` (and adjacent closing braces), which the proofs show never
  run; the branch CBMC adds to a call through a function pointer for a target the harness never
  gives it (the verifier's replay visitor), which it closes with `assume(false)`; and the lines
  `unreachable_lines` in `prove.sh` names for a proof, each with its reason:
  `display_width`'s width overflow checks need a span of more than 2^63 bytes; the portable
  resolver cannot begin a step on a separator after its caller and previous step removed them.

CI runs the proofs and the reach check side by side, `MO_CBMC_JOBS` at a time each.
`ci/check.sh cbmc` sets that to half the processors, so a machine with many processors and
little memory can run out: on 14 processors and 16 GB, the kernel killed `step`'s proof and its
reach check. A proof or reach check whose `cbmc` is killed says so, and names the command that
reruns it alone; it is not a failed proof.

## What the proofs assume

These neighbours are models, each with its own checks:

- **The arena** (`src/base/arena.c`) hands out each block as its own object of exactly the size
  asked, and may fail any allocation. `arena.c` proves that the real arena's blocks are as large
  as asked and overlap no other block, which is what makes this model faithful.
- **The name table** (`src/bytecode/names.c`) gives each string a canonical index that is absent
  or no later than its own, which is all the reader relies on.
- **The validator** (`mo_bytecode_validate`) may accept or refuse anything; a refusal has a reason
  and an offset inside the input. `mo_case_shape` is only asserted to be called with a decoded
  type and case.
- **`mo_assert_failed`** asserts, so a failed `MO_ASSERT` fails the proof (CBMC would treat a
  plain `abort` as a path that never happened).
- **`memcpy` and `memchr`** are plain byte loops: CBMC's library has no `memchr`, and its
  `memcpy` is very slow with a symbolic offset and length.

## What the proofs do not check

- That the reader accepts exactly the right inputs: `tests/test_bytecode_io.c` and the fuzz target
  check what it decodes. A proof here holds for any refusal with a reason.
- That an enum field holds one of its values. After refusing an unknown layout kind or anchor
  expectation, the reader still stores the number it read, so `prove.sh` leaves out CBMC's
  `--enum-range-check`.
- Inputs larger than the sizes above, and the name table and validator themselves.

## The arena

`arena.c` proves each public function of `src/base/arena.c` from **any arena the arena's own
functions can leave behind**: any limit and any bytes used within it, up to `SIZE_MAX` (`mo run
--heap-size` takes any size), a next chunk size between the first and the largest, and a newest
chunk of any capacity the system can give with any part of it used. Sizes and counts are any
value up to `SIZE_MAX`, and alignments any power of two up to `MO_ARENA_MAX_ALIGNMENT`. For each,
the function:

- reads and writes only inside its chunks, never overflows or wraps a size, and never fails an
  `MO_ASSERT`;
- leaves an arena that is still one (`arena_ok`) with the same limit;
- hands out a block of at least the size asked, aligned as asked, inside the newest chunk and after
  every block handed out before it, or a new chunk linked to the old one and counted in what the
  arena used;
- fails only when the block would not fit in a new chunk within the limit, or the system has no
  memory; and when it fails, changes nothing, and a failed resize leaves the block as it was.

| Proof | Also checks |
| --- | --- |
| `arena_allocate` | the new block ends where the chunk's free space starts, which `mo_arena_resize` relies on to grow the newest block in place |
| `arena_allocate_array` | a table whose size in bytes wraps is refused, for any count and element size; that the block holds the whole table, for any count and the element sizes 0 to 4, 8, 12, 16, 24, 40, 48, 64 and 4096 (for any element size, CBMC would have to show two 64-bit products equal, which does not finish); `mo_arena_allocate` is its model |
| `arena_resize` | any byte the block keeps, in place, grown alone or moved, is the same after; shrinking never fails and keeps the block where it is; only the newest block grows in place; a block in the newest chunk ends below its free space |
| `arena_copy`, `arena_copy_text` | any byte of the copy is the source's, and a text's terminator is copied; texts of up to 8 bytes |
| `arena_free` | every chunk of an arena of up to three goes back to the system (`--memory-leak-check`), and the arena is empty with the same limit |

The system is modelled: `malloc` and `realloc` may fail, always fail for more than 2^40 bytes (so
that CBMC can represent each chunk as one object), and align what they return to 16 bytes, as the
chunk layout needs on our targets. `memcpy` asserts that it reads and writes only inside its
objects, and copies one byte, the one the harness picked as any byte of the block; copying a
symbolic number of bytes of a block of up to 2^40 is far too slow for CBMC.

Not proved: a system that hands out more than 2^40 bytes at once, and a 32-bit `size_t` (CBMC
runs these proofs for 64-bit targets).

## The verifier

`verify.c` proves the verifier's transfer functions: what it does with **any one instruction of
any opcode**, with any operands the validator accepts, in any small file the validator accepts.
A file has three types, two functions, up to two layouts, constants and globals, up to three
registers per function, and up to eight code words; the function verified is the first; and a
variable operand list has up to three operands, one more than any table, so every count check
can fail.

| Proof | Checks |
| --- | --- |
| `instruction` | `mo_verify_instruction`, the type check: reads only inside the file's tables, never fails an `MO_ASSERT`, and either accepts with the error untouched or refuses with a reason at the instruction. When it accepts, each count the later passes and the VM loop over matches what they index with it: a call's arguments its function's parameters, a closure call's its function type's, a primitive's or capability operation's its arity, a construct's or reuse's fields its layout's, a closure's captures its function's, a switch's targets its choice's cases, and a case made is one of its choice's, with that case's fields |
| `step` | the ownership transfer of one instruction (the loop body of `ownership`, split out as `step`), from any register states, any layout `cases` gave a reset and any room left in the undo log: it reads only inside its tables and either refuses with a reason inside the function's code, or accepts only when the ownership rules hold (each register read is readable, each consumed reference owned and each consumed token a token, no reference used again after it is consumed, no write over an owned register or a token, a tail call borrows only borrowed references, a return leaves nothing owned, an init reads only globals set before it and sets its own once) and then leaves each register in the state the rules give, logs fewer changes than the instruction has words, and the log, undone newest first, gives back every slot; the readable mask a replay reports matches the states |
| `lookup` | `mo_verify_layout_lookup`: any answer is a layout of that type and tag, and in sorted keys a layout that is there is found |

The proofs assume what the validator (`src/bytecode/validate.c`) checks: every operand inside the
table its kind names, every target an instruction start, an environment type names a closure, and
a function's parameters have its registers' types. `step` assumes what `ownership` gives it: each
block runs once, so no register is yet marked as used by the instruction, and a visit keeps the
readable mask matching the states. Its operands' access modes come from the real `access`, which
`step`'s proof runs but does not specify. Two library checks are not proved, and may accept or
refuse anything: `mo_bytecode_signature` (`src/bytecode/signature.c`), only asserted to be given
decoded types, and `mo_bytecode_ordering`. `step`'s files have no parameter, field or case tables,
which it never reads; a read would fail the proof.

Not proved: the three passes that drive these functions (`graph`, `cases`, `ownership`) and
`function`, which runs them. Their loops over blocks and edges, even for eight code words, give
CBMC more than this machine's 15 GB. `tests/test_bytecode_verify.c` and the verifier fuzz target
check them. `mo_case_shape`, which only `cases` calls, has a model so that the file links. The
proofs do not show that the type check accepts exactly the right programs: it may refuse more
than it should, and only the counts above are checked of what it accepts.

## The primitives

Four files prove the arithmetic and the UTF-8 that Mo's primitives rest on, each against a
specification written independently of the code: in wider arithmetic for numbers (CBMC's
`__CPROVER_bitvector` types for 256-bit values), and from the Unicode Standard's table of
well-formed byte sequences (table 3-7, `utf8.h`) for UTF-8. Every proof also shows that its
function never overflows, wraps a conversion, shifts out of range, divides by zero, reads outside
its span or fails an `MO_ASSERT`.

| Proof | Checks |
| --- | --- |
| `text_scalar` (`text.c`) | the decoder behind `Bytes.decode_utf8` and every Text walk accepts exactly the well-formed sequences, from any offset in any span of up to 6 bytes, with the value each encodes and the offset past it; a refusal gives no value |
| `text_encode`, `unicode_encode` | every scalar value is spelt as the one well-formed sequence that encodes it; `encode(c, NULL)` counts what it spells |
| `checked_scalar` (`unicode.c`) | the same as `text_scalar` for the decoder of spans that may be malformed, and a refusal changes nothing |
| `unicode_scalar` | the decoder for texts, which are well-formed, gives each sequence's value and length |
| `properties` | the binary search for a scalar's Unicode properties stays inside its table and gives the properties of the range holding the scalar, or none for a scalar in no range, for any scalar value |
| `next_grapheme`, `display_width` | `mo_unicode16_next_grapheme` and `mo_unicode16_display_width` succeed exactly on well-formed spans of up to 6 bytes (a cluster also on any span with a well-formed first cluster); a cluster is whole scalars and ends before one; a width is at most two columns per scalar. `properties` is their model, giving any properties |
| `word_product`, `multiply` (`decimal.c`) | the 256-bit product of two magnitudes is the sum of the products of their 64-bit halves in place; the half products are C's own multiplication, which `word_product`'s proof bounds by their operands' sizes |
| `divide_step` | one step of long division keeps 2 × remainder + bit = quotient bit × denominator + new remainder, and the remainder below the denominator |
| `round_quotient` | rounds to nearest with ties to even, gives the sign, and refuses exactly when the result is outside Decimal's range, leaving the output alone |
| `decimal_mul`, `decimal_div` | `mo_decimal_mul` and `mo_decimal_div`, for any Decimals (any nonzero divisor), divide the right product by the right denominator with the right sign: \|a\| × \|b\| over 10^18, or \|a\| × 10^18 over \|b\|, negative when exactly one operand is; a refusal leaves the output alone |
| `truncated_division` (`number.c`) | C's division, for any Int over any Int but 0 and -1, has a remainder below the divisor with the dividend's sign, and a quotient of at most 2^62 either way |
| `int_primitive` | every Int primitive but `pow`, on any two Ints, gives the reference's result: `abs` overflows exactly at `INT64_MIN`, and `floor_div` exactly for `INT64_MIN / -1`; `floor_div` and `floor_mod` refuse a zero divisor, and otherwise floor C's truncating quotient and remainder exactly; `wrapping_` gives the exact result's low 64 bits, `saturating_` clamps it, `checked_` gives `Some` exactly when it fits; `to_decimal`, `to_float` (rounded as C rounds), `to_big` (least significant byte first, none zero at the top), `to_byte`, `to_char` (exactly the scalar values) and the five `Duration` constructors (microseconds, overflowing exactly when out of range) |
| `exact_product`, `int_multiply` | `mo_prim_exact_product`, the one 128-bit product the Int primitives take, is at most 2^126 either way; `Int.pow`'s checked multiplication multiplies a by b through it, refuses exactly when the product does not fit, and otherwise gives it |
| `int_pow` | `Int.pow` for any Int and exponent: refuses a negative exponent with a reason, gives 1 for a zero one, multiplies at most twice for each bit of the exponent, and overflows exactly when a multiplication does |

Decimal multiplication and division are proved in pieces because CBMC cannot show two
multiplier or divider circuits equal in reasonable time (each attempt ran past fifteen minutes).
The loop that chains the division's steps, `rounded`, is not proved by CBMC: carrying "the
remainder is below the denominator" through its 256 steps takes CBMC's solver time exponential in
the steps (eight already take over a minute). It is argued instead. The remainder starts at 0,
below any denominator, and `divide_step` keeps it below, so each step meets the next one's
requirement. By induction, before the step for bit k the quotient and remainder are those of the
numerator's bits above k, since each step keeps 2r + bit = q·d + r'; after the last they are the
whole numerator's, so `round_quotient` rounds the exact value once. Each bit index is below 256,
so `bit / 64` indexes one of the four words and `bit % 64` is a valid shift.

Likewise, `int_primitive` takes C's truncating `/` and `%` from `truncated_division`'s model,
whose properties (a remainder below the divisor with the dividend's sign, a quotient of at most
2^62) that proof shows, and checks the floor adjustment exactly; that the quotient is a / b is C's
own arithmetic. It takes each product from `mo_prim_exact_product`'s model, checking that the primitive multiplies a by the right factor
(b, 10^18 for Decimal, or a Duration unit in microseconds) and what it does with the product.
`int_pow` replaces the checked multiplication of `prim.c` by a model that may refuse, so it shows
the control of square-and-multiply, not the value of a^n or which multiplications run, which
`tests/test_prim.c` checks (`(-2).pow(63)` fits, `2.pow(63)` overflows).

Models: the heap's builders (`mo_value_case` for an Option, `mo_value_bigint`, `mo_value_text`)
check what they are given and may run out of memory; printing may give any text or fail.

Not proved: which scalars join into a grapheme cluster, and each one's width, which follow the
Unicode data tables (`ci/check_unicode16.py` runs the standard's conformance tests); the
Decimal primitives' rounding to places, `floor`, `ceil` and `to_int`, `Float.to_decimal`, BigInt,
the Text primitives built on the decoder, and the collections, whose cost CBMC cannot bear
through 128-bit dividers or the heap. The tests and the VM-against-Lean differential check them.

## The host paths

`path.c` proves `mo_host_normalize` against architecture section 7 ("Files"): split on `/`, skip
`.`, pop on `..`, and refuse an empty path, an empty component (absolute, trailing slash, or
`//`), or a `..` that would leave the root. Real and fake Files operations run this before they
open or look up a path (sandbox-properties P8).

The proof takes **every path of up to `MO_CBMC_PATH` bytes** (6 by default) whose bytes are `.`,
`/` or `a`. `a` stands for any other component byte: the function only special-cases `.` and `/`.
For each such input it accepts exactly when the spec does, writes the spec's bytes, and a refusal
is `FILE_NOT_ALLOWED` with `containment`. It also never overflows, reads past the path, or fails
an `MO_ASSERT`. `files.c` is included with `MO_CBMC_NORMALIZE` defined, so only
`mo_host_normalize` is compiled from that file.

`resolve.c` includes the real `src/host/path.c` with an **eight-byte path buffer, two link
expansions, eight work steps, and a root plus three temporary descriptors**. The portable
proof takes every relative path of up to seven bytes over `{'.', '/', 'a'}`, every mode and
every initial work count from zero to its limit. Link targets range over the same alphabet,
up to the full eight-byte `readlinkat` buffer, including truncation. The Linux wrapper proof
also takes an eight-byte input to exercise its length refusal; other inputs are canonical
paths that normalization can leave, including the empty path.

| Proof | Checks |
| --- | --- |
| `resolve_portable` | memory, pointer, conversion and arithmetic safety of the component walk, link expansion and root-based ancestor replay; syscalls use live root-derived descriptors, never ask for `..`, never pass a slash-containing component, and open with `O_NOFOLLOW`; the shared work count stays within its limit and never decreases |
| `resolve_kernel` | the real Linux resolver and `mo_host_resolve` wrapper, with portable resolution replaced by its proved contract; `openat2` always requires `RESOLVE_BENEATH` and `RESOLVE_NO_MAGICLINKS` and pins with `O_PATH`; I/O reopening uses only a retained `/proc/self/fd/` descriptor and cannot create a pathname |

Both proofs keep the root open and leave exactly the successful output descriptor live, or no
temporary descriptor on failure. Each bound has an unwinding assertion. Reach checks cover
the functions above and the harness ends, except the portable loop that skips leading slashes:
inputs and link targets are relative, and each component advance already skips separators.

The OS is a model, not the subject of these proofs. Opens, inspection, link reads and directory
creation can fail. Successful opens allocate a free descriptor; descriptor identities and file
kinds are arbitrary, so replacements and identity mismatches are included. `close` releases its
descriptor and succeeds. The Linux syscall may return `EINTR` once, so its retry body runs;
unbounded interruptions are not proved. `snprintf` models the exact retained-descriptor format
for the small descriptor set. Byte-copy models check both buffers and copy all requested bytes,
including overlapping moves. Error translation preserves `ENOENT` and otherwise returns failure.

Not proved: production-size paths or work/link limits, arbitrary descriptor numbers, libc or
kernel internals, exact filesystem outcomes or error messages, concurrent namespace snapshots,
or that the portable resolver implements whole-lookup ancestry rather than directory-object
authority. These harnesses target 64-bit Linux; non-Linux directory-open flag branches are not
proved. The kernel proof trusts the OS meaning of `RESOLVE_BENEATH`; the portable proof
checks the calls that preserve retained authority, not an actual filesystem graph. NUL in an
input path is refused by callers before normalization and is not included here.

## Reference count updates

`refcount.c` includes the real `heap.c`, `cell.c` and `types.c`. Counts range over **every
nonzero 32-bit value**, including 1, `UINT32_MAX - 1` and the immortal `UINT32_MAX`.

| Proof | Checks |
| --- | --- |
| `refcount_dup` | `mo_value_dup` returns the same cell, increments a reference's count exactly once, saturates at the immortal count, and leaves an immediate's count and the object's layout alone |
| `refcount_enqueue` | `enqueue_object`, used by drop, decrements a non-immortal live object's count exactly once, queues it exactly when its count reaches zero, preserves the previous pending tail, and changes neither the queue nor the link otherwise; null and immortal references do nothing |

The duplication harness uses Int, Text and four choice cases covering empty, inline Int,
reference Text and boxed two-field payloads. Type classification and cell accessors are real,
not models. Both proofs also check memory safety, conversions and arithmetic with the runner's
usual checks. `--reach` reaches every block of both update functions and `mo_value_object`.

Not proved: arbitrary type graphs, the destructor's walk over child references, allocation and
release, reset and reuse, or the compiler's placement of updates. A pending queue is empty or
has an existing tail distinct from the live object. A zero count is not a legal input to an
update. These are local transition proofs, not a proof that the whole heap cannot leak.

## Hash-table probing

`table.c` includes the real `heap/table.c` and `cell.c`. Hashes range over every 64-bit value,
including collisions. Keys and values are arbitrary Int cells. The runtime masks hashes before
narrowing them to slot indices, so conversion checking does not need an exception for intentional
hash truncation. This leaves the slot calculation unchanged.

| Proof | Checks |
| --- | --- |
| `table_layout`, `table_layout_limit` | map and set cells, hashes, live flags, slots and trailer fit in the allocation without overlapping, at capacities 4 and 128 with twice as many slots; allocation may fail |
| `table_insert` | returns the first empty slot after the hash, including wraparound, or refuses a full eight-slot index; entries and tombstones are both occupied |
| `table_probe_limit` | at any start in 256 slots, accepts an empty slot on probe 128 and refuses when the first empty slot needs probe 129 |
| `table_lookup`, `table_lookup_set` | in eight slots with at most four nonempty slots, returns the first matching entry or absence, the first reusable tombstone for absence, and the exact probe distance; equality failure leaves outputs untouched |
| `table_compact` | up to two entries in a four-entry map become a dense live prefix in insertion order, with unchanged keys, values and hashes, each indexed once and reachable before an empty slot; no stale slot or tombstone survives |
| `table_union` | plans up to three insertions in eight slots, using the first vacancy not reserved by an earlier key, including tombstones, collisions and overlay collisions; refuses a full index |

The layout proofs use the real allocation accessors and a model allocator that returns an exact
block or fails. The probing proofs replace those accessors with separate typed arrays of the
same sizes. This avoids CBMC's byte-array alias circuits while retaining the bounds and region
separation established by the layout proofs. `memmove` checks its source and destination bounds
and copies one entry through saved cells, including overlap. Equality models exact Int equality
or failure. The private overlay hash is an arbitrary deterministic function of its slot input.

Every loop bound has an unwinding assertion. `--reach` checks the proved functions and harness
ends, including insertion and union refusal. The half-full assumption on lookup excludes its
terminal `MO_UNREACHABLE`, not a legitimate lookup result.

Not proved: larger table shapes beyond the separate 256-slot probe-limit case, arbitrary key
types or hashing/equality implementations, allocation/rebuild/retry orchestration, or whole
collection operations. These are local probing and compaction proofs, not a proof of the heap.

## Running it

`.agents/setup` installs CBMC from the distribution (Ubuntu 24.04 ships 5.95.1), and CI runs it
as the `cbmc` group of `ci/check.sh`. A failed proof prints its failing properties; the full log
is `build/cbmc/<proof>.log`. Adding `--trace` to the `cbmc` command `prove.sh` runs, on
`build/cbmc/<proof>.goto`, shows a counterexample. To cover larger inputs, set `MO_CBMC_INPUT`;
the time grows with it.
