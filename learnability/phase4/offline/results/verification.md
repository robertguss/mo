# Verification record for the offline diagnosis work

Each section names the check, its command and its result. Failed attempts stay beside the passes.
The predictions these results answer are in `predictions.md`.

## Baseline

`bash ci/check.sh gcc` on `origin/main` (830f6b18) plus the plan and design commits, before any
code changed: `check.sh finished: pass`.

## Signature matching and the fix-check verdict

Tests: `tests/test_verdict.c`. It has 26 classification rows covering each matching condition of
design section 6.2 and each field it compares or ignores, 24 verdict rows covering every cell of
section 7.1, the three acceptance examples run through both steps, a symmetry check, and the
record constructor. Expected values were written by hand from the design tables.

- Red: a stub answering false or refused everywhere failed 57 checks
  (`make -j8 build/gcc/debug/tests/test_verdict.test.run CC=gcc`, "verdict: 57 check(s) failed").
- Green: the same target under gcc and clang prints "verdict: all checks passed". The full
  `make test` passes under both compilers, and `make format-check`, `make deps` and `make tidy`
  are clean.

Mutants. Each was built, run and reverted; the prediction was that each fails at least one row.

| Mutant | Result | Rows that failed |
| --- | --- | --- |
| Verdict ignores the original | 13 checks failed | every "different outcome" and "not reproduced" verdict row, and the heap acceptance example |
| Defect matching ignores the span | 4 checks failed | different span file, start and end, and the defect symmetry check |
| Memory matching accepts any limit | 6 checks failed | heap against stack, depth and native allocation at the same value, native allocation against heap, same name with a different value, and the memory symmetry check |
| A failed size check counted as success | 1 check failed | reproduced, returned, with the state failing the size check |
| Memory compares value but not name | 5 checks failed | the four same-value name rows and the memory symmetry check |

Two facts found here bear on later work. A failed size check is a `state_size` outcome, not a
separate flag, so the model's `sizeOk` is "the status is not `state_size`". The VM names two
native limits, `native memory` (`src/vm/process.c`, `src/vm/execute.c`) and `native allocation`
(capture, when no limit is named); exact matching keeps them apart.

## Query entry check

Tests: `query_entries` in `tests/test_check.c` and `test_source_query` in `tests/test_lower.c`,
on a two-module program. Accepted: a bare name, `Main.name`, an overloaded `Store.tick` resolving
to its `State` overload, an unexposed `Store.peek`, and failure queries for a tool and a timer.
Command mistakes: an unknown module, an unknown function, a type name, and an imported rather than
declared bare name. Refused with a code: two generic entries (one whose first parameter is a type
parameter, which would otherwise hit the purity walk's assertion), a capability inside a parameter
record, a function-typed parameter, functions inside a list result, a same-shape state type,
extra parameters on a state query, swapped, missing and extra failure-query inputs, and a program
with no service. The new codes are E1410 to E1413; until `mo query` exists they are pinned by
`tests/errors/unit.tsv` against these unit tests.

- Red: a stub that always refused failed 36 `test_check` checks; a stub lowering output returning
  nothing failed 2 `test_lower` checks.
- Mutants, each reverted, each failing the tests: ignoring argument order, taking the first
  overload, using the service wording, checking generics after purity (aborts on the assertion),
  and letting a state query take extra parameters.
- Green: `make test` under gcc and clang, `python3 ci/check_errors.py`, `make deps`, `make tidy`
  and `make format-check`.

## Lean: saved-schema round trip and the verdict

`formal/MoCore/Saved.lean` and `formal/MoCore/Verdict.lean`, checked by `make formal` and by
`ci/check_lean_axioms.sh` in the `lean` group.

- Saved: `decode_encode` (a fitting value round-trips), `decode_checks_schema` and
  `decode_refuses_other_schema` (decode accepts only the expected schema bytes), and
  `decodeInputs_encodeInputs` (state, arrival time and arguments round-trip as one tuple).
- Verdict: `signaturesMatch_iff` (the matching function equals the design's matching relation),
  `verdict_verified_iff` and `verdict_sound`, `verdict_not_verified_of_not_reproduced` (for every
  revision), and `verdict_table` (every row of the design's verdict table). The C rows in
  `tests/test_verdict.c` and `verdict_table` cover the same cells.
- Negative controls: `verdictRevisedOnly_unsound` (a verdict accepting a successful revision alone
  is not sound) and `decodeBlind_ignores_schema` (a decoder ignoring the schema fails the refusal
  property), each with a `#guard` on its concrete counterexample.
- Axioms: every theorem pins its `#print axioms` output with `#guard_msgs`; none depends on
  anything beyond `propext`, `Classical.choice` and `Quot.sound`. A planted `sorry` in
  `verdict_verified_iff` showed `sorryAx` there and in the three theorems using it, and every
  guard failed. Removing a pin, admitting `sorryAx` in a pin, and a real `sorry` each made the
  script exit 1.
- Red during development: an unfinished `reproduced_iff` showed `[propext, sorryAx]` and its guard
  failed the build.

Disagreement found and resolved: the model first ignored the file when matching defects while the
C matcher compared it. A span locates code only within its file, so the model, and design table
6.2, now compare the file too (`make formal` and the axiom check pass after the change). Design
6.2 also named one saved-bytes limit for `state_size`, while capture records three saved-copy
limits; the table now says name and value, as both implementations match.

Not modelled, and listed in `formal/README.md`: that different schemas have different bytes,
recursive schemas, and a byte-for-byte comparison of the Lean and C encoders.

## Artifact reader, argument walk and type schema digest

Tests: `tests/test_artifact.c` (new) and `tests/test_saved_state.c`. Kind-1 and kind-2 artifacts
come from a real running service with a failing tool taking `Int` and `Text`; the reader returns
the same identity as `mo_export_verify`, spans equal to the stored bytes, a state that decodes to
the live state, and arguments 5 and "five" in order. Every strict prefix, every single flipped
bit, resealed payload and checksum changes, a changed schema length and an extra byte before the
digest are refused with the output unchanged. The task queue's state type digest equals
`mo_export_schema` over its encoded state; a renamed field, an added field and a reordered choice
each change it.

- Red: stub functions failed every reader and digest check; `fuzz_fetch` on a new kind-2 seed
  failed its kind-1-only assertion before the fix.
- Mutants, each failing: no check that the body ends at the digest (first caught only by CBMC; an
  extra-byte unit test was then added and fails too), ending the walk whenever the count is spent,
  not decrementing the count, no rollback in the schema writer, a digest one byte short, and
  swapped kind-2 spans.
- Fuzz: `make fuzz-smoke FUZZ_SECONDS=5` for `fuzz_artifact`, `fuzz_fetch` and `fuzz_export`;
  `fuzz_artifact` for 180 s (3.55 million runs) with no crash. The first smoke run found a bug in
  the fuzz oracle (an empty label's span), kept as the `empty-label` seed.
- CBMC, recorded in `cbmc/README.md`: `mo_export_read` up to 172 bytes, 1,851 properties, 87 to
  101 s, every block reached; `mo_failure_argument_next` up to 32 bytes, 281 properties, every
  block reached. The first reach check showed the fixed 66-byte prefix could never be refused after
  the length check, so the reader now reads it at fixed offsets. Kind-2 frames are modelled, not
  proved, since a minimal kind-2 artifact exceeds the bound; fuzzing and unit tests cover them.
- Green: `make test` under gcc and clang, sanitize builds of the four affected tests, `make deps`,
  `make format-check`, `make tidy`, `ci/check_fuzz_seeds.py`.

## Store lock wait and exit 75

Tests: `tests/test_service_store.c`, built with a 1-second wait. A store held for 0.3 s by a
second serve or a reader opens after the wait and recovers state 18. A store held past the wait
gives the busy status three times in a row with the file's bytes unchanged, and the next open after
release recovers. The open without the wait refuses at once, and an empty or garbage store fails at
once. The CLI driver's second `mo serve` on a live folder now exits 75 after the real wait
(9.5 to 25 s observed) with the line naming a second serve or a reader, store bytes unchanged.

- Red: mutants with no busy timeout and with busy mapped to IO each fail these tests.
- Green: `make test` under gcc and clang; `python3 ci/check_cli.py build/gcc/debug/mo` (guide claims
  for the E1409 line and the new exit 75 line included); `make deps`, `make format-check`,
  `make tidy`.

## Log record layout and the read-only reader

Characterization first: on the layout moved to base, before any reader code, `test_recovery` and
`test_service_store` passed; `ci/check_kill_loop.py` passed short and with `--seed 7 --kills 60`
(6 kills mid-commit); a binary built before the move and one built after each recovered the
other's data folder. `tests/test_log_record.c` pins hand-written layout bytes, the walk's refusals
and writer-then-walk round trips; five walk mutants are caught. `fuzz_log_record` ran 1.06 and
0.85 million inputs with no failure.

Reader tests (they failed to link before the reader existed):

- a stopped folder's snapshot, tail entries and failures equal what the service wrote;
- a folder another process holds is owned, and refusal is immediate;
- a writer killed after its pages reached the database leaves a hot journal: recovery required,
  and with recovery the journal rolls back and the store reopens on the earlier state;
- a service killed while idle leaves a `-journal` file that is not hot and reads normally;
- a store without failure tables reads as no records and gains no table;
- an unreadable file, an unenterable folder (real `chmod`) and injected `CANTOPEN` and `PERM` are
  unreadable; a missing file is no saved data and none is created; a garbage file is damaged;
- a damaged log row is refused naming its sequence;
- a shortened 64 KiB history bound reports what was read and left;
- a reader killed mid-copy leaves no lock;
- without recovery, the database file's bytes never change.

Eleven reader mutants are caught, among them always opening read-write, skipping the checksum,
ignoring the bound, ignoring recovery, and not classifying busy or rollback codes. Kill loop with
`--seed 11 --kills 40` passes.

Prediction that did not hold as written: "a real process killed mid-transaction is recovery
required" holds only once database pages were written. In exclusive locking mode a write killed
before any page reaches the database leaves a zeroed journal header, which SQLite correctly treats
as not hot, since the database is intact. The test kills a writer whose 1 MiB append overflows the
page cache.

## The offline call, shared with capture

`mo_service_call_handler` now carries the call, the new-state step, the saved copy and the
work-limit poll for every caller; `restore_record` and `mo_service_step` use it. Capture and replay
share `mo_service_signature` and `mo_service_context`. `mo_offline_call` runs one handler on a
record's inputs in a fresh process under the service limits, and `mo_service_budgets_match` names
the first budget that differs from the build's own.

- Characterization: before any edit, `make test CC=gcc` and the kill loop passed; after the lift,
  before any new test, the service runtime, service store, recovery, recovery-fault and verdict
  tests passed unchanged.
- New tests, in `tests/test_service_runtime.c` and `tests/test_service_store.c`: a captured `stop`
  in a read-only and in an updating tool replays with a signature equal field by field and context
  text equal byte for byte, `reproduced`, state and log unchanged, and the same stop one line lower
  is `different`; the work limit at the exact boundary is OK on trial 0 and cancelled on trials 1
  and 2, as live, with the same frames and context; the full-mailbox memory setup fails live and
  succeeds offline, `not_reproduced`; state-size failures of an updating tool and a timer replay as
  `state_size`, and a fixed revision succeeds while one returning `Error` succeeds with an error
  reply and the state unchanged; the budget check; injected allocation faults end as a refusal or a
  memory failure; too few or too many arguments are refused.
- Red: every new test passed on its first run, since the behaviour already held after the lift, so
  mutants of the production code stood in. Each failed its test: skipping the saved copy, "state
  changed" always false, ignoring `Error` replies, dropping variables from the context, the frame
  walk reporting the wrong function, a shrunk offline heap, ignoring the arrival time, a +2 poll
  formula, leaving the heap out of the budget check, and removing the trailing-argument check (it
  survived until a mismatched-argument test was added). A process leak on the refusal path is
  caught by LeakSanitizer.
- Green: `make test` under gcc and clang, the clang sanitizer build of the six affected tests,
  `make format-check`, `make deps`, `make tidy`, the kill loop.

The base outcome struct was renamed `mo_run_outcome`: its old tag collided with the VM's
`enum mo_outcome`, so no file could include both headers.

## CBMC: the log record walk

`cbmc/log-record.c`, registered in `cbmc/prove.sh` as `mo_log_record_walk` at 37 bytes (the
25-byte header plus three empty arguments, so the loop accepts several arguments and refuses a
later length cut short). The reader callback may fail any read after writing arbitrary bytes, and
must be asked for exactly the next expected range. An oracle computing the fixed offsets with
64-bit sums decides acceptance exactly; on refusal every output field, all 254 sizes included,
equals its value before the call.

- `bash cbmc/prove.sh mo_log_record_walk`: proved, 0 of 381 properties failed, 9 to 11 s
  (rerun on the integrated branch: 11 s). `--reach`: every block reached, 5 to 6 s.
- Mutants, each failing the proof: a length bound relaxed by one (read-inside-payload, read-order,
  pointer-bounds and unwinding assertions fail), and `offset != length` weakened to
  `offset > length` (the exact-acceptance assertion fails).
- Failed attempt kept: with CBMC's own `memcpy` and the walk's symbolic copy size the problem had
  27 million variables; the 37-byte run was killed after more than 10 minutes and a 29-byte run hit
  its 600 s timeout. A byte-loop `memcpy`, as `cbmc/resolve.c` uses, brought it to about 10 s.
- The reach check now also accepts CBMC's "replacing function pointer by" message for the
  unreachable branch it adds at a constant function-pointer call, still at most one let-off per
  line. Not covered at this bound: the argument-count limit, since 37 bytes cannot hold enough
  arguments to reach it; `tests/test_log_record.c` tests it.

## The commands: mo replay, mo replay --fix, mo query, mo history

- Flag table rebuilt as one row per flag, characterized first: a dump of `mo_cli_parse` over
  959,727 generated argument lists is byte-identical before and after.
- Refusals: the checks run in the fixed order before anything is decoded; each names the check and
  both values and exits 1. A record with no build identity has its own reason, and `mo history`
  lists it as not replayable. `mo query` takes `--max-work`, `--max-heap`, `--max-stack` and
  `--max-depth`; each stop names its limit and exits 70. JSON totals carry `status` equal to the
  exit status. E1410 to E1413 moved from `tests/errors/unit.tsv` to `tests/errors` cases.
- Red runs seen before fixes: a fix adding a state field failed because the revision's schema was
  computed in FILE's heap (the revision now gets its own process); the history JSON test failed
  because refusals also went to stderr; two guide claims failed on the worker's own guide edits.
- Mutants: seven of `src/tools/offline.c` (budgets, argument schema, handler, verdict, build,
  revised shape, program) each fail `ci/check_offline.py`; two parser mutants each fail
  `tests/test_cli.c`.
- Green on the integrated branch: `bash ci/check.sh gcc drivers static` passed, including
  `ci/check_offline.py`, `ci/check_cli.py` with the guide claims, `ci/check_errors.py` and the
  conformance check.
- Not exercised at the command level: the queued-mailbox memory failure that does not reproduce.
  A real `mo serve` cannot fill the mailbox through MCP (20 calls a second, 16 KiB bodies), so that
  outcome is proved at the VM level only.
- Measured: the task queue's largest state (10,461,125 saved bytes) renders to 10,922,848 bytes of
  JSON, which sizes `MO_LIMIT_OFFLINE_VALUE_BYTES` at 16 MiB, with a 144 MB peak; `mo history` on it
  took 220 s in the `-O0` debug build. The store is closed before rendering, so the time never holds
  the lock.

## Fuzzing the whole history reader

`fuzz/fuzz_history.c` builds one store file per input, starting from an empty store made by
`mo_store_open`, then adds a snapshot, log rows and failure rows as well-formed frames with fuzzed
payloads or raw blobs, and may flip bytes, move sequences and ids, drop state rows, or change the
head and `user_version`. A model in the fuzzer predicts each read's status and damaged sequence
exactly, and for a successful read every entry, the snapshot, each failure, the retained ids, and
the retained and left byte counts; the file's bytes must be unchanged after both reads. It is built
with a 4 KiB history bound so small inputs reach the limit, the way the store unit test shortens
it. Nine seeds.

- Found: a failure record with an id of zero or below made the history read succeed while
  silently leaving the record out, and the pair read answer OK or damaged depending on the id
  asked for. The catalog reads ids above a cursor starting at 0, and the by-id lookup never sees
  other rows. Fixed with one clause in the shared failure-pair check in `src/host/store.c`, so
  every failure read, including the running service's failure view, now refuses such a store as
  damaged. The runtime never writes such ids, so only damaged files are affected.
  `history_nonpositive_failure` in `tests/test_service_store.c` failed before the fix (the history
  read and the pair read for id 1 both returned OK), as did the fuzzer's `nonpositive-id` seed.
- Runs: `make fuzz-smoke FUZZ_SECONDS=5` over all 38 targets with no artifacts (`fuzz_history`
  1,718 executions); `fuzz_history` alone for 180 s, 34,383 executions, peak 461 MB, no crash, leak
  or leftover folder. Green: `make test` under gcc, `test_service_store` under clang debug and
  sanitize, `make format-check`, `make deps`, `make tidy`, `ci/check_fuzz_seeds.py`.
