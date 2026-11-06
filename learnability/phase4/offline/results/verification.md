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
