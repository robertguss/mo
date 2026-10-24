# Mo sandbox properties

What the Mo toolchain guarantees about a program it loads and runs, where each guarantee is
enforced, and what evidence backs it. The format follows Wasmtime's security documentation: one
numbered property at a time, each with its enforcement point, its evidence, and what is not
covered. A property with no enforcement or evidence is listed under "Gaps", not claimed.

This document states facts that other documents define. Where they disagree, those win:
`docs/design/bytecode.md` (section 9 for verification), `docs/design/architecture.md` (7 for
capabilities, 8 for running, 11 for testing) and `docs/design/implementation.md` (the five
conditions). Nothing here is new behaviour.

## The threat model

The untrusted input is a **bytecode file** (a program compiled from source, or one that arrives
from elsewhere, as upgrade packages will in Phase 5) and the **Mo program it encodes**. The
toolchain must not let either one read or write memory the VM did not give it, reach the outside
world except through a capability it was handed, or use more stack, call depth or heap than the
limits allow.

The **trusted code** is the C that implements the loader, the verifier, the VM, the primitives
and the host (`src/bytecode`, `src/vm`, `src/prim`, `src/host`, `src/heap`, `src/base`).
C is not memory safe, so for this code the five conditions of `implementation.md` replace a
language guarantee: sanitizers, fuzzing, Tiger Style assertions, a small trusted core, and a
verified bytecode format. The compiler (`syntax`, `check`, `lower`, `core`, `codegen`) is **not**
trusted by the VM: the VM never sees source or Core, and the verifier checks the bytecode no
matter what produced it (`bytecode.md` section 1).

Out of scope: a host operating system that is already compromised, other processes changing the
files a capability reaches while a lookup runs (P8), and timing or other side channels.

## Evidence kinds

| Name | What it is | How it runs |
| --- | --- | --- |
| Verifier | `src/bytecode/validate.c`, `verify.c`, `verify-instruction.c`, `verify-program.c`: refuses a file at load time | every load |
| Assertion | `MO_ASSERT`, never compiled out (`implementation.md`, condition 3) | every run |
| Unit test | `tests/test_*.c`, through public headers | `bash ci/check.sh gcc` |
| Sanitizers | the unit tests under ASan and UBSan | `bash ci/check.sh sanitize`, by hand |
| Fuzz target | `fuzz/fuzz_*.c` with seeds in `fuzz/corpus/` | `bash ci/check.sh fuzz`, by hand |
| CBMC proof | bounded model checking of small functions, `cbmc/` (`cbmc/README.md`) | `make cbmc`, by hand |
| Differential test | the C VM against the Lean evaluator of Mo Core, `ci/check_core_differential.py` | `bash ci/check.sh lean`, by hand |

Mo has no hosted CI. Only the unit tests (`bash ci/check.sh gcc`) are a routine pre-PR check, plus `drivers` and
`static` when a change reaches them (`AGENTS.md`, "Checks and merging"); the other groups run
when someone runs them. A property whose only evidence is a by-hand group is still backed, but
nothing re-runs it for you.

## Pair assertions

Tiger Style asks that a fact be checked at the point it is established and again at the point it
is used. For the sandbox, the verifier establishes a fact about the bytecode at load time, and
the VM asserts the same fact where it relies on it. If the two ever disagree (a verifier bug, a
VM bug, or memory corruption between load and run), the process stops at an assertion instead of
carrying on with a broken invariant. The pairs, and where each half lives:

| Fact | Verifier | VM |
| --- | --- | --- |
| An instruction's register operands exist | `validate.c` `operand()`; `verify.c` `step` asserts it for read operands | `execute.c` `read`, `write`, `consume` |
| Move and dup keep the type | `verify-instruction.c` "move/dup type mismatch" | `execute.c` `MO_OP_MOVE`, `MO_OP_DUP` |
| A constant or global named exists and has the register's type | `verify-instruction.c` `MO_OP_CONST`, `MO_OP_GLOBAL` | `execute.c` `MO_OP_CONST`, `MO_OP_GLOBAL` |
| A global is stored only by an init function, once | `verify-instruction.c` "global store outside init", `verify-program.c` ranks | `execute.c` `MO_OP_SET_GLOBAL` |
| Every global has exactly one initializer, so all are set after initialization | `verify-program.c` rank loop | `image.c` `initialize` |
| Every block start is an instruction start inside the function | `validate.c` (`starts[]`, `MO_OPERAND_TARGET`); `verify.c` `graph` asserts the entry is a block | `execute.c` `MO_OP_JUMP`, `MO_OP_BRANCH`, `MO_OP_SWITCH` and the pc check |
| A branch tests a Bool; a switch has one target per case | `verify-instruction.c` "branch needs Bool", "switch target count" | `execute.c` `MO_OP_BRANCH`, `MO_OP_SWITCH` |
| A call's argument count, argument types, result type and target kind match its callee | `verify-instruction.c` `MO_OP_CALL`, `MO_OP_CALL_CLOSURE` | `execute.c` call cases |
| A closure's environment sits after the parameters, and closure code is entered only as a closure | `verify.c` `ownership`, `verify-instruction.c` | `process.c` `mo_vm_enter` |
| A return has the function's result type and leaves no owned reference | `verify-instruction.c` "return type", `verify.c` `step` | `execute.c` `MO_OP_RET`, `clean` |
| A construct, reuse, make_case or make_closure has the layout, case and field count it names | `verify-instruction.c` | `execute.c` constructor cases |
| A primitive or host operation is called with its table's arity and modes | `verify-instruction.c` "primitive arity", "host arity" | `execute.c` `MO_OP_PRIM`, `MO_OP_HOST` |
| A register written is not an owned reference or token (no overwrite leaks) | `verify.c` `step` "write overwrites ownership or token" | `execute.c` `write` |
| A consumed reference is owned, and a frame's owned count reaches zero at return | `verify.c` `step` "consume needs an owned reference", "return leaves ownership" | `execute.c` `consume`, `clean` |

## Properties

### P1. A loaded file is well formed

*A file the loader accepts has every count, index and offset inside its table and limit, valid
UTF-8 strings, valid constants, and well-formed types and layouts.*

- Enforced by: the reader (`src/bytecode/read.c`), `validate.c`, and the structure and type
  checks of the verifier (`bytecode.md` section 9, items 1 and 2). A refusal carries a reason
  and an offset inside the input.
- Evidence: CBMC proves the reader memory safe, integer safe and assertion free for every input
  up to `MO_CBMC_INPUT` bytes (`cbmc/reader.c`); `fuzz/fuzz_bytecode_io.c` and
  `fuzz/fuzz_bytecode_text.c` run larger inputs; `tests/test_bytecode_io.c`,
  `tests/test_bytecode_text.c`.
- Not covered: the proof is bounded (32 bytes by default), and does not show the reader accepts
  exactly the right inputs (`cbmc/README.md`, "What the proofs do not check").

### P2. Every operand has the type its instruction needs

*No register is read as a type other than the one it holds, and no field, case or layout is
used out of range: a verified program has no type confusion.*

- Enforced by: `mo_verify_instruction` (`verify-instruction.c`), using `prims.def` and
  `hostops.def` signatures and ability checks (`bytecode.md` section 9, item 4). The VM does
  not branch on a value's kind; it relies on this, and asserts the kinds of the few operands it
  reads by kind (`kind_of` in `execute.c`).
- Evidence: CBMC `instruction` proof: for any one instruction of any opcode, the function reads
  only inside the file's tables, never fails an assertion (as of the last CBMC run; see below), and accepts only when the counts
  later passes and the VM index with (arguments, fields, switch targets, captures) match their
  tables (`cbmc/verify.c`). `tests/test_bytecode_verify.c`; `fuzz/fuzz_bytecode_verify.c`.
  The VM's pair assertions (table above) check the same facts as it runs. This change added
  assertions to `mo_verify_instruction` and `step`; the `instruction` and `step` proofs were not
  re-run with it (CBMC was unavailable), so they are pending a run of `make cbmc`.
- Not covered: CBMC shows what the check accepts has the right counts, not that it accepts
  exactly the right programs; it may refuse more than it should. Type agreement of operands is
  tested and fuzzed, not proved.

### P3. Memory use is safe: no use after free, double free, leak or read of an undefined register

*A verified program frees each reference exactly once, never uses one after consuming it, and
never reads a register that is undefined or moved.*

- Enforced by: the ownership dataflow (`verify.c` `ownership` and `step`; `bytecode.md`
  sections 4.2 and 9, item 5). Every read and consume follows the ownership states; states agree
  at every join; `ret` and tail calls leave nothing owned. `stop` and `fail` may leave owned
  registers because the process heap is freed whole when a process ends (`architecture.md`
  section 8).
- Evidence: CBMC `step` proof of one instruction's ownership transfer from any register states:
  it accepts only when the rules hold and leaves the states the rules give, and the undo log
  restores every slot (`cbmc/verify.c`). `tests/test_bytecode_verify.c`. `fuzz_bytecode_verify.c`
  loads arbitrary and mutated files, and for each accepted file runs only its zero-parameter,
  non-closure, non-init functions (`verifier_fuzz_select_entries`), at most
  `VERIFIER_FUZZ_MAX_CALLS` of them, under an instruction budget, and asserts that no object
  remains in the heap after a call that returns; `fuzz_vm.c` does the same for
  generated valid bytecode. The VM asserts the states as it runs (`read`, `consume`, `write`,
  `clean` in `execute.c`). The unit tests also run under ASan and UBSan.
- Evidence for count updates: `cbmc/refcount.c` proves duplication and drop's enqueue transition
  on every nonzero 32-bit count, including immortal saturation and enqueueing exactly on the
  transition from one to zero. It covers immediate, reference and four choice payload shapes.
  It does not prove the destructor's child walk, allocation, release or reuse.
- Not covered: `graph`, `cases` and `ownership`, the passes that drive `step`, are not
  CBMC-proved (their loops exceed the memory CBMC has); they are tested and fuzzed. The claim
  holds "given a correct VM, primitives and host" (`bytecode.md` section 9): a bug in the C
  runtime can still corrupt memory, which is what sanitizers and fuzzing are for.

### P4. Control flow stays inside the function

*Every jump, branch and switch lands on the first word of an instruction in the same function;
no path falls off the end; every instruction is reachable.*

- Enforced by: `validate.c` and `graph` in `verify.c` (`bytecode.md` section 9, item 3). Infinite
  loops are allowed: this is not a termination check (see P9).
- Evidence: `tests/test_bytecode_verify.c`, `fuzz_bytecode_verify.c`. Pair assertions in `graph`
  and in the VM's jump, branch, switch and instruction-fetch sites.
- Not covered: not CBMC-proved (same reason as P3).

### P5. Loading is bounded in time and memory

*The verifier refuses, rather than runs away on, a file that needs too much work or scratch
space.*

- Enforced by: `MO_LIMIT_FUNCTION_REGISTER_BLOCKS` (registers times blocks, at most 2^22),
  `MO_LIMIT_VERIFY_WORK_PER_WORD` (128 steps per code or register word plus a header),
  `MO_LIMIT_VERIFY_BYTES_PER_WORD` (2,048 scratch bytes per word), explicit stacks in every
  graph walk, and linear-time ability and initialization summaries (`bytecode.md` section 9).
  Exceeding a ceiling is a load error at the instruction where it is crossed.
- Evidence: the "verifier work" scaling checks in `tests/test_bytecode_verify.c` (for example the
  65,535-register unused-choice regression), `fuzz_bytecode_verify.c`.
- Not covered: the limits are checked by tests, not proved.

### P6. Globals are initialized once, in dependency order

*Every global has exactly one initializer, no initializer reads a global that is not yet set
(directly or through direct calls), and only initialization runs `init` code.*

- Enforced by: `verify-program.c` (rank per global, summaries over the call graph), the
  `set_global` and read checks in `step` (`bytecode.md` section 9, item 6, and 3.2), and the
  rule that no code calls an `init` function.
- Evidence: `tests/test_bytecode_verify.c`; the VM asserts a global is set once and that every
  global is set after initialization (`execute.c`, `image.c`).
- Not covered: reads reached through unknown closure calls are not charged by the verifier. The
  VM keeps a runtime guard for them: an unset global read stops the process with an internal
  error (`bytecode.md` section 9). That guard is a stop, not a refusal at load.

### P7. The outside world is reachable only through capabilities

*A program can read files, print, read the clock, draw random numbers, or read arguments and
the environment only through a capability value it was given, and `main` receives only the
capabilities it declares.*

- Enforced by: the language has no other way to name an effect; every effect is a `host`
  instruction through a capability object's operation table (`architecture.md` section 7;
  `src/base/hostops.def`). Real capabilities are made once by the runtime for `main`
  (`mo_call_main`, `src/vm/process.c`); fake constructors, `printed`, `printed_errors` and
  `fail` are accepted only in spec code, and non-spec code cannot call, tail-call, make a
  closure of, or load a closure constant of spec code (`bytecode.md` section 9, item 6). So
  `main` and what it reaches cannot make a fake, even in an image that contains tests.
  `Files.within` and `read_only` only narrow a capability.
- Evidence: `tests/test_host.c`, `tests/test_fakes.c`, `tests/test_bytecode_verify.c`;
  `fuzz/fuzz_host.c` and `fuzz/fuzz_host_real.c`.
- Not covered: the primitives (`src/prim`) are trusted C that the VM calls directly; that they
  perform no I/O is by construction and review, not a mechanical check. The host operations are
  trusted C. Console and entropy sources are process-wide: `mo_host_start` ignores `SIGPIPE`
  for the whole process (`architecture.md` section 7).

### P8. File capabilities cannot reach outside their folder

*A `Files` or `ReadFiles` handle resolves paths beneath the directory object it holds, and
nothing else, through `..` or links.*

- Enforced by: lexical normalization that refuses absolute paths, empty components and a `..`
  that leaves the root; then `openat2` with `RESOLVE_BENEATH` where available, or a
  component-at-a-time `openat` with `O_NOFOLLOW` and link expansion bounded by
  `MO_LIMIT_PATH_LINKS` and `MO_LIMIT_PATH_STEPS`; identity checks of the resolved object before
  I/O (D236; `architecture.md` section 7, "Files").
- Evidence: CBMC `normalize` proof: `mo_host_normalize` accepts exactly the contained relative
  paths of up to 6 bytes over `{'.', '/', 'a'}`, and a refusal is a containment failure
  (`cbmc/path.c`); `tests/test_host_resolver.c` and `tests/test_host.c` (including hostile links);
  `fuzz/fuzz_host_real.c` (path resolution never resolves outside the folder, `architecture.md`
  section 11).
- Not covered: another process renaming an intermediate directory during a lookup can make the
  lookup follow that object to its new location; Mo's guarantee is directory-object authority,
  as in Capsicum, WASI and cap-std, and does not make the namespace an atomic snapshot
  (`architecture.md` section 7). The portable and `openat2` resolvers in `src/host/path.c` are not
  CBMC-proved; they remain tests and fuzz.

### P9. Stack, call depth and heap are bounded

*A process uses at most `--max-stack` bytes of stack (default 8 MiB), `--max-depth` calls in
progress (default 10,000) and `--max-heap` bytes of heap (default 1 GiB). Past a limit, the
process stops with a defect report naming it. It does not crash or grow without bound.*

- Enforced by: `mo_vm_reserve` and the call path in `execute.c` and `process.c`; the heap's own
  accounting (`src/heap`); `MO_LIMIT_VALUE_BYTES` for a single value
  (`architecture.md` section 7). Frame sizes are computed from `register_count`, itself limited by
  `MO_LIMIT_FUNCTION_REGISTERS`. Arenas, not scattered `malloc`, hold compiler and verifier data
  (D207).
- Evidence: `tests/test_vm.c`, `tests/test_vm_faults.c` (allocation faults injected by
  compile-time renaming), `tests/test_heap.c`, `tests/test_limits.c`; `fuzz/fuzz_vm.c` (an
  outcome, never a crash); CBMC proves the arena hands out blocks of the size asked, overlapping
  none, and fails without changing anything (`cbmc/arena.c`).
- Not covered: **time**. Verification does not check termination, and a default `mo run` has no
  instruction or wall-clock limit. An embedder can bound a run with `mo_process_instruction_limit`
  or stop it from outside with `mo_call_controlled`'s `keep_running` callback (used for
  promises and in `src/tools/builders.c`); the fuzz targets use the instruction budget. Native memory outside the heap (the
  stack buffer's `realloc`) can still fail; that case stops with "running out of memory".

### P10. A verified program that goes wrong stops cleanly

*Overflow, a division by zero, an index outside a list, and similar are defects: the process
stops, prints a report from state that is still readable, and frees its heap whole.*

- Enforced by: `mo_vm_stop` and `defect.c`; the report prints only registers that are value,
  owned or borrowed at the stopping instruction, found by re-running the verifier's dataflow
  (`bytecode.md` section 9), so it never reads a consumed register.
- Evidence: `tests/test_vm.c`; `fuzz_vm.c` `replay()` compares the VM's states with the readable
  mask, only at a non-returning stop and only on the top frame; `fuzz_bytecode_io.c`
  `check_readable` checks, at every instruction, that the streaming visit agrees with a
  single-instruction replay (verifier against verifier, not against VM states); the Lean
  differential test compares outcomes, including stop messages, with Mo Core's evaluator (`ci/check_core_differential.py`).
- Not covered: the differential test runs generated programs without capabilities.

## What the trusted base is

A property above holds "given a correct VM, primitives and host". Those are the parts a memory
bug could break, so they are the parts the other evidence aims at:

| Part | Size and kind | Evidence |
| --- | --- | --- |
| Loader and verifier | `src/bytecode`, C | CBMC (reader, `instruction`, `step`, `lookup`; `instruction` and `step` not re-run after MO-58's new assertions), fuzzing, tests, sanitizers |
| VM | `src/vm`, C | Pair assertions, fuzzing (`fuzz_vm.c`), differential test against Lean, sanitizers. **No CBMC proof of the interpreter loop.** |
| Heap and arena | `src/heap`, `src/base/arena.c`, C | CBMC (arena, local reference count updates), `fuzz_heap.c`, sanitizers |
| Primitives | `src/prim`, C | CBMC (arithmetic and UTF-8), `fuzz_prim.c`, oracles in `tests/test_prim.c` |
| Host | `src/host`, C | CBMC (`normalize` of `mo_host_normalize`), `fuzz_host.c`, `fuzz_host_real.c`, resolver tests |
| Standard library | `lib/`, Mo | memory safe by construction (condition 4) |

## Gaps

These are not guaranteed today.

1. **No termination or time bound by default.** See P9.
2. **No proof of the VM loop, the verifier's drivers, the heap, or path resolution after
   normalize.** CBMC covers the reader, the arena, the verifier's per-instruction functions, the
   arithmetic and UTF-8 primitives, local reference count updates, and lexical path containment
   (`mo_host_normalize`). The heap's destructor walk and allocation remain unproved. The rest
   has tests, fuzzing, sanitizers and (for the VM) the Lean differential test.
3. **The slow checks do not run on their own.** Sanitizers, fuzzing, Lean and CBMC run only when
   someone runs them (`AGENTS.md`, "Checks and merging"); no hosted CI runs any check. A
   regression in them is found when they next run.
4. **No operating-system confinement.** Nothing in `src` installs seccomp, pledge, Landlock or a
   similar filter. A bug in the C runtime is not contained by one. Capabilities are the only
   boundary the toolchain provides.
5. **No concurrency checking.** There is no concurrent code yet, so no ThreadSanitizer run
   (`implementation.md`, condition 1); property P7's process-wide signal handling and any future
   threads need their own review.
6. **Concurrent changes to the filesystem** are outside P8.
7. **Verified file, not verified compiler.** A compiler bug produces a program that the verifier
   accepts but that does not mean what its source says. That is a correctness bug, not a
   sandbox escape; the Lean differential test and the golden corpus look for it.
8. **Assertions are evidence of intent, not proof.** A pair assertion stops the process when a
   fact is false at run time. It does not show the fact is always true, and it costs a check
   per use. The cost was measured once, on `tests/test_vm.c` at `-O0` (0.47 s before, 0.48 s
   after); it was not measured with optimization.

## Keeping this document true

A change that adds or removes an instruction, a limit, a capability operation, a fuzz target or a
proof updates the property it affects. The facts here are checkable: the pair-assertion table
names functions in `src`, the evidence rows name files in `tests/`, `fuzz/` and `cbmc/`.
