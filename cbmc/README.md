# CBMC proofs

`make cbmc` (or `bash cbmc/prove.sh [proof...]`) proves the bytecode reader,
`src/bytecode/read.c`, with [CBMC](https://www.cprover.org/cbmc/). For **every input of up to
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
`layout_entry`, `module_entry`, `file_entry`, `debug_entry`), which keeps every proof small.

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
| `sections` | every section in order, the reader's objects unchanged | every function above |
| `decode` | `mo_bytecode_decode_info`: header, checksum, trailing bytes, types' references | `allocate`, `sections` |

Loops are unwound to bounds the input size implies (a table of entries of at least 4 bytes has
at most `MO_CBMC_INPUT / 4` of them), and every bound carries an unwinding assertion, so a bound
that is too small fails the proof rather than hiding paths.

A proof can also hold because part of its function never runs: an input too small for a loop's
body, or an assumption that rules out the paths that matter. Two checks catch that:

- **Input sizes.** Each proof runs on inputs large enough for each of its loops to run once:
  `functions` needs 40 bytes (a function entry takes 36), `debug_entry` 52 and `sections` 64.
  `prove.sh` refuses a proof whose input leaves a loop bound below 2.
- **Reach.** `bash cbmc/prove.sh --reach` runs CBMC's location coverage on each proof and fails
  unless every block of the proved function runs on some input, and so does the end of its
  harness. The only blocks allowed to stay unreached are the failure branches of `MO_ASSERT`,
  which the proofs show never run.

CI runs the proofs and the reach check side by side, `MO_CBMC_JOBS` at a time each.

## What the proofs assume

These neighbours are models, each with its own checks:

- **The arena** (`src/base/arena.c`) hands out each block as its own object of exactly the size
  asked, and may fail any allocation. Its own proof is task V5.
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
- Inputs larger than the sizes above, and the arena, name table and validator themselves.

## Running it

`.agents/setup` installs CBMC from the distribution (Ubuntu 24.04 ships 5.95.1), and CI runs it
as the `cbmc` group of `ci/check.sh`. A failed proof prints its failing properties; the full log
is `build/cbmc/<proof>.log`. Adding `--trace` to the `cbmc` command `prove.sh` runs, on
`build/cbmc/<proof>.goto`, shows a counterexample. To cover larger inputs, set `MO_CBMC_INPUT`;
the time grows with it.
