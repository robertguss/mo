# Fitting the repository's programs to the gates

The log of U11 in `docs/plans/2026-11-18-2013-feat-trust-gates-mo-test-plan.md`: how the 12 library
modules, the 12 examples and the task queue were made to pass `mo test --gates`, from `main` at
`82b7df1f`, with the table of kinds frozen at `5cabd4ff`. `docs/status/trust-gates-run.md` holds
the times and the decisions. One agent session did the work on 2026-11-19, with three helpers
of the same model, one for `lib/json.mo` and two for the examples; every program was then run
again by the session itself.

## What failed before

The task queue had the 8 survivors of the dry run. `lib/json.mo` and `examples/account.mo` failed
stated intent, so their mutants had not been tried; once they stated intent, they showed 30 and 4
survivors. In all, 104 survivors and 4 functions with no stated intent.

| Program | Before | After |
| --- | --- | --- |
| `lib/json.mo` | E1440 on `Json.to_text@JsonNumber`, then 30 survivors | 0 |
| `lib/list.mo` | 3 survivors | 0 |
| `examples/account.mo` | E1440 on `open`, `owner` and `balance`, then 4 survivors | 0 |
| `examples/expenses.mo` | 6 | 0 |
| `examples/find.mo` | 1 | 0 |
| `examples/inventory.mo` | 2 | 0 |
| `examples/job-queue.mo` | 6 | 0 |
| `examples/json-query.mo` | 23 | 0 |
| `examples/kv.mo` | 4 | 0 |
| `examples/notes-tool.mo` | 4 | 0 |
| `examples/rate-limit.mo` | 7 | 0 |
| `examples/todo.mo` | 2 | 0 |
| `examples/wc.mo` | 4 | 0 |
| `services/task-queue/queue-tests.mo` | 8 | 0 |

The other 10 library modules and `examples/greet.mo` passed already.

## Mutants and functions with no mutant

Counted from `mo test --gates --list --json` before and after.

| Program | Mutants before | Mutants after | Functions | With no mutant before | With no mutant after |
| --- | ---: | ---: | --- | ---: | ---: |
| 12 library modules | 274 | 265 | 70, then 70 | 31 | 31 |
| 12 examples | 273 | 266 | 100, then 99 | 36 | 37 |
| task queue | 86 | 85 | 42, then 42 | 18 | 18 |
| All 25 | 633 | 616 | 212, then 211 | 85 | 86 |

By program, the counts that changed: `lib/json.mo` 229 to 220, `examples/expenses.mo` 18 to 14,
`examples/inventory.mo` 16 to 14, `examples/rate-limit.mo` 31 to 30 and the task queue 86 to 85.

Functions that lost all their mutants:

- `Inventory.pad_column`: its two mutants were in `or 0`, a width default no input reaches.
- `RateLimit.limiter_of`: deleted (below).

## Survivors no check could kill

Each of these changes nothing a check can observe in the program as it was written. Each was
resolved by simpler code that behaves the same for every input; no check was weakened or
deleted.

- `lib/json.mo`, `Json.skip_space`, `.code > 32` to `.code > 33`: the only character it moves is
  `!`, which then takes the slow path, which returns at once. The test is now
  `(chars.get(at) or 'x') > ' '`, a Char comparison with no Int literal.
- `lib/json.mo`, `Json.plain`, `point <= 0` to `<`, and `point >= digits.size` to `>`: at each
  boundary both branches give the same digits. The three-way branch is now one padding, then a
  `take` and a `drop` at `point.max(0)`.
- `lib/json.mo`, `Json.plain`, `kept > 0` to `>=`: `fraction.get(-1)` is `None`, so the guard is
  redundant and is gone.
- `examples/expenses.mo`, `Expenses.layout`, both width defaults `or 0` to `or 1`: with no rows no
  line is printed. The two widths are read with one `case`.
- `examples/inventory.mo`, `Inventory.pad_column`, `or 0` to `or 1`: the cells always hold the
  title. The width is read with a `case`.
- `examples/rate-limit.mo`, `RateLimit.limiter_of`, `per_second: 2` to `3`: the helper was used
  only on both sides of the promise's anchors, and the promise never reads `per_second`. The
  helper is gone and the anchors name the limiter in full.
- `services/task-queue/queue-tests.mo`, `QueueTests.job`, page limit `1` to `2`: the helper kept
  only `.jobs.first`, the same job for either limit. It now takes the page's only job and stops
  otherwise, the change the fitting session of U10 made to the frozen copy; a limit of 2 now
  stops a test.

Three more were judged unkillable by a check of the result, because they only send a string
down the slow walk, which gives the same answer: `Json.controls`, `0..32` to `0..33`;
`Json.string_at`, `source.controls.empty? or clean?(…)` to `and`; and `Json.clean?`, its last
`true` to `false`.

The helper fitting `lib/json.mo` first removed these fast paths. That made `Json.parse` take
49,582,823 instructions on the 1 MiB body of Phase 7's bar, against 28,842,519 on `main`
(`learnability/phase7/preflight/json-parse/run.sh`), which would miss the bar of 2 seconds, so
the change was undone. A new test parses a document holding one string of 60,000 characters
with no escape, and the gate kills all three mutants by its step limit: on the slow walk the
test takes more than four times its steps. With `clean?`'s `true` made `false` by hand, the test
still passes under plain `mo test`, so the step limit is what kills it. The parser as fitted
takes 28,140,943 instructions on that body, and the two parsers print the same line for each of
the 425,758 documents of the corpus.

## Survivors only one call sequence killed

None. The dry run judged one survivor of that kind: `Main.current?`, `until > now` to
`until >= now`, which a service sequence kills only with a call at the exact instant a lease ends.
`current?` is a helper of the service promises, so an example on it kills the mutant directly:
`not current?(leased_job, t0 + lease_length, 1, 1)`. `Main.matching`, the other promise helper
with a survivor, got an example the same way.

## How each survivor was killed

Every other survivor was killed by a new example, test or assert.

- `lib/list.mo`: one example on `sort_by_desc`, `[3, 4, 1, 2]`, kills all three (a two-element
  half left unsorted, and a merge that skips an element).
- `lib/json.mo`: an example on `to_text@JsonNumber` for stated intent; four examples on `plain`
  at the 60-digit limits; one on `quoted` for a control character's `\u00` escape; asserts in the
  existing tests for a NUL in a string, `false`, a parsed document's position, an exponent, an
  empty object after another, a surrogate pair with mixed-case hex digits, and three bad
  surrogate escapes; and the long-string test above.
- `examples/account.mo`: one example each on `open`, `owner` and `balance`, and four on
  `deposit` and `withdraw` at amounts of 0.50 and at the 1,000,000,000.00 limit.
- `examples/expenses.mo`, `find.mo`, `inventory.mo` and `wc.mo`: a new test of `main` through the
  fakes for each, pinning exit statuses, the usage error and the report.
- `examples/job-queue.mo`: examples on `submit`, `lease` and `fail`, including a `fail` at the
  instant a lease ends.
- `examples/json-query.mo`: seven examples on `parse`, one each on `hex_value` and `quoted`, and a
  test of `main`'s exit statuses.
- `examples/kv.mo`: an example on `apply` for a delete, and three on `expire` at the ttl limits.
- `examples/notes-tool.mo`: three examples on `search_notes` at limits 1, 100 and 101.
- `examples/rate-limit.mo`: four examples on `new_limiter` at its two limits, one on
  `wait_for_one`.
- `examples/todo.mo`: one example of `Remove` on the first item.
- The task queue: two examples on `JobQueue.submit`, one on `JobQueue.lease` with a lease of one
  second, one on `JobQueue.expired` at the third attempt, two on `Main.current?` and one on
  `Main.matching`.

Kills by kind of check after fitting, over the 25 programs, are in each program's gated `--json`
run; the task queue's last full run killed 1 by a promise, 28 by examples, 30 by tests and 22 at
start.
