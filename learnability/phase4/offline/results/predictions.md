# Predictions for the offline diagnosis checks

Written on 2026-11-06, before any check below ran. Each line names the check that will show it.
A check that turns out differently is recorded in `verification.md` beside this prediction; this
file is not edited afterwards.

## Store lock wait

- A second process holding the store for less than the wait: `mo serve` starts and recovers the
  same state. Check: store unit test with a shortened wait.
- A lock held past the wait: the open reports busy, `mo serve` exits 75, and the store's bytes are
  unchanged. Check: store unit test, and the CLI driver's second-serve case at the real 10 seconds.
- A damaged or zero-length store still fails at once with E1409 and exit 1. Check: existing E1409
  cases, unchanged.

## Read-only reader

- A stopped folder: the snapshot sequence, each tail entry's sequence, handler index, timer flag,
  arrival and argument sizes, and each failure's id and kind key equal what the service wrote.
  Check: store unit test, and the acceptance driver against an independent SQLite reader.
- A live owner is classified as owned and returns at once; a hot journal is recovery required; a
  service killed while idle leaves a `-journal` file that reads normally. Check: store unit tests
  and the kill loop.
- A store without failure tables reads as holding no records, creates nothing, and keeps its bytes.
  Check: store unit test.
- Without `--recover` the database file's bytes never change. Check: store unit test.

## Artifact reader and schema digest

- Kind-1 and kind-2 artifacts from real captures read to the same identity `mo_export_verify`
  returns, and their spans decode to the captured state, message and arguments. Every strict prefix
  is refused. Check: export unit test and `fuzz_artifact`.
- The schema digest of a type equals the digest of the schema inside an encoding of a value of that
  type, and a renamed field, an added field and a reordered choice each change it. Check:
  saved-state unit test.
- The CBMC harnesses over the artifact header walk, the failure argument iterator and the log
  record walk pass with their reach checks. Check: `bash cbmc/prove.sh` and `--reach`.

## Replay

- A captured `stop` failure replays to `reproduced`, with a signature equal field by field to the
  stored one and context text equal to the stored text, from the folder and from the artifact.
  Check: service runtime unit test and the acceptance driver.
- A work-limit failure at exactly the service work limit stops offline at the same poll. Check:
  service runtime unit test.
- A handler that ran out of heap live because the mailbox held large queued arguments succeeds
  offline: `not_reproduced`, and `--fix` with the unchanged handler is `unverified`. Check: service
  runtime unit test and the acceptance driver.
- Every existing recovery, capture and runtime test passes unchanged after the call path is lifted.
  Check: `bash ci/check.sh gcc`.

## Verdict

- Each row of the design's verdict table gives its verdict; a reproduced original with a revision
  that fails the size check is `not_fixed`; an `Error` reply is `verified` with reply kind `error`.
  Check: verdict unit test.
- Three mutants each fail at least one row: a verdict that ignores the original, matching that
  ignores the span, and matching that accepts any memory limit. Check: verdict unit test run
  against each mutant.

## Query entry

- A capability inside a parameter record, a function-typed parameter, and a function inside a list
  result are each refused with a coded diagnostic; a generic entry is refused before the purity
  walk. Check: check unit test and `tests/errors` cases.
- A query program that only adds a function is accepted; a renamed state field is refused with both
  schema digests. Check: CLI unit tests and the acceptance driver.

## Commands

- Each compatibility check is refused on its own by a fixture breaking only that check, naming the
  field and both values. Check: acceptance driver.
- A revision that adds a state field is refused before the original runs. Check: acceptance driver.
- A query that exceeds `--max-work` stops naming `--max-work` and exits 70. Check: acceptance
  driver.

## Lean

- The saved-schema round trip, the schema refusal, verdict soundness and completeness and the
  matching relation build with no axiom beyond `propext`, `Classical.choice` and `Quot.sound`.
  Check: `bash ci/check.sh lean`.
- A planted `sorry` makes the axiom check fail; the revised-only verdict and the schema-blind
  decoder are each refuted by a concrete counterexample. Check: `bash ci/check.sh lean`.
- Defect replays of `stop`, integer overflow and division by zero agree between `mocore` and
  `mo replay`. Check: the offline differential driver.

## Grader cross-check

- On the synthetic variant's retained `stats` failure: the unfixed program is `not_fixed`, the
  reference fix is `verified` and passes the grader, and the broken fix is `verified` but fails the
  grader. Check: `learnability/phase4/test_offline.py` in `bench`.
- On the six private variants, every reference fix is `verified`; any broken fix that removes the
  failure is also `verified` and fails the hidden tests. Check: by hand, recorded in the PR.

## Measurements

- The reader's lock on the largest task-queue folder (5,000 jobs of 2,048 bytes) lasts well under
  the 10-second serve wait, so a history read never makes `mo serve` exit 75. Check: the
  measurement script in this folder.
