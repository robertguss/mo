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
