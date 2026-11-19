# Trust gates: the dry run and the measurement

The record of PR3 of `docs/plans/2026-11-18-2013-feat-trust-gates-mo-test-plan.md`: U8, the dry
run that settles the table of kinds, and U10, the measurement of how many of the 24 wrong
versions a task queue fitted only to the gates kills. The result files are in
`learnability/trust-gates/`, which its `README.md` describes.

Robert delegated every decision in this work on 2026-11-19 ("I trust you to make all decisions
for me"; "Don't stop for any approval from me for anything"). Where the plan says Robert
approves or answers, the decision below is marked "decided by the agent under Robert's
delegation of 2026-11-19", with its reasons. No number here decides whether the gate is kept
(R25).

## Predictions

Written on 2026-11-19, before `mo test --gates` or `mo test --strength` ran on any version of the
task queue, and before the gate ran on any library module or example in this session. Earlier
sessions that built PR1 and PR2 ran the gate on fixtures under `tests/cli/`; I have seen no
result of a gate run on these programs. Plain `mo test` was run on the frozen and the current
task queue to measure the slowest check, which `--seconds` needs. What I had read: the Phase 6
strength results (`learnability/phase6/strength/results/`), the wrong versions' mistakes in its
manifest, and the sources of the programs. The build is the debug build, the one the nightly
uses.

### The dry run (U8)

The programs are the 12 modules of `lib/`, the 12 programs of `examples/` and
`services/task-queue/queue-tests.mo` as they stand on this branch.

- D1. The checks stage passes on all 25 programs.
- D2. The stated-intent stage fails (E1440) on at least 8 of the 12 library modules, since most
  expose functions with no example. It passes on at least 8 of the 12 examples, most of which
  expose nothing or already have examples, and on the current task queue, whose seven tools
  each have a promise `on` it or on its handler.
- D3. The task queue has between 350 and 700 mutants, against the plan's crude count of about
  650 places to change. Across all 25 programs there are between 1,500 and 3,500.
- D4. About a quarter of the tested functions (between 15% and 40%) have no mutant.
- D5. The Int literal row and the arithmetic row make the most mutants; `not` makes the fewest.
- D6. Where the mutant stage runs, the Int literal row has the highest share of survivors (at
  least 30%), the boundary row the next highest, and the logic, `not` and Bool rows the lowest
  (under 15%).
- D7. No row's sampled survivors are mostly unkillable, so all seven rows stay (70% sure). If
  one drops, it is the Int literal row.
- D8. The gated run of the current task queue takes between 20 and 90 minutes; the 24 other
  programs take under 10 minutes together.

### The proof (U10)

- P1. Before fitting, `--strength` through the frozen `queue-tests.mo` kills 3 of 12 of the
  working half by promise or example, the same versions as Phase 6's `1-before.json`, and 5 of
  12 by any check (between 4 and 7). The held-back half: between 3 and 7 of 12 by any check,
  most likely 5.
- P2. The fitting session makes `mo test --gates queue-tests.mo` pass (75% sure), in 2 to 5
  hours.
- P3. After fitting, `--strength` kills 9 of 12 of the working half by any check (between 7 and
  11) and 8 of 12 of the held-back half (between 6 and 11). Most new kills come from promises
  and examples on the tools, fewer from new tests.
- P4. At most one wrong version becomes uncountable after fitting (changes intent or does not
  check).
- P5. Fitting adds checks rather than changing code, so the mutant count and the functions with
  no mutant stay the same or change by under 5%. No function loses all its mutants.
- P6. The fitting log has between 0 and 3 survivors the agent judged no check could kill, and
  between 1 and 5 that only one particular call sequence killed.

## The dry run (U8)

Run on 2026-11-19 from commit `04a62857` with the debug gcc build, one program at a time in
two lanes: the task queue in one, the 24 library modules and examples in the other, beside the
two frozen `--strength` runs of U10. `learnability/trust-gates/count.py` counts the table below
from `learnability/trust-gates/results/dry-run/`; mutants made come from `--gates --list`, the
rest from the gated run's `mutant` records.

| Program | Functions | With no mutant | Mutants | Killed | Survived | Dropped | Exit | Seconds |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- | ---: |
| 12 library modules | 70 | 31 | 274 | 40 | 3 | 2 | 1 (`json` E1440, `list` E1439) | 7.0 |
| 12 examples | 100 | 36 | 273 | 188 | 59 | 10 | 1 on 11 (`account` E1440) | 17.3 |
| `services/task-queue/queue-tests.mo` | 42 | 18 | 86 | 73 | 8 | 5 | 1 (E1439) | 1,015.4 |
| All 25 | 212 | 85 | 633 | 301 | 70 | 17 | | 1,039.7 |

| Row | Made | Killed | Survived | Dropped | Survivors among those run |
| --- | ---: | ---: | ---: | ---: | ---: |
| boundary | 65 | 28 | 16 | 0 | 36% |
| negate | 130 | 76 | 1 | 0 | 1% |
| logic | 56 | 20 | 1 | 16 | 5% |
| not | 10 | 4 | 0 | 0 | 0% |
| arithmetic | 111 | 51 | 10 | 1 | 16% |
| bool | 38 | 24 | 3 | 0 | 11% |
| int | 223 | 98 | 39 | 0 | 28% |

Kills by kind of check: example 174, test 100, start 24, promise 3. On the task queue alone:
test 29, start 22, example 21, promise 1. A "start" kill is a mutant after which the program
no longer starts, such as one in a constant.

`lib/json.mo` (229 mutants) and `examples/account.mo` (16) fail stated intent, so their mutants
are listed but not tried; the rows' kill and survival columns leave them out. Eight library
modules have no mutant: `set`, `text`, `time` and `unicode16` have no `fn` of their own, and
`file-error`, `files`, `option` and `result` have no code the table changes.
Plain `mo test` takes 75.5 seconds on the frozen task queue and 79.3 on the current one, almost
all of it `QueueTests.test.22` (71.4 and 72.7 seconds); the gated run is 12.8 times plain.

Two defects in the instruments turned up and were fixed before the table was frozen:

- `mo test --list --json` crashed on `lib/json.mo` and `examples/json-query.mo`: the intent
  walk recursed forever through the ability proofs of the recursive type `Json`. This was on
  `main` before this branch. Commit `04a62857` stops the walk at a proof already on its path;
  digests of every other program are byte-identical to `main`'s (checked on `lib/list.mo`,
  `lib/option.mo`, `examples/kv.mo`, `examples/todo.mo` and both task queues). The crashed
  listings are kept as `*.list.crashed.json`; the gated runs of both programs did not crash.
- Review of U9 found that a wrong version whose test stopped was classed `defect` and left the
  rate, and that a failing test hid an unfinished promise. Both were fixed in `ea0b1c19`
  before any `--strength` run here.

### The predictions against the dry run

- D1, right: the checks passed on all 25.
- D2, partly wrong: stated intent failed on 1 of 12 library modules, not at least 8; most
  library functions with no example are not exposed or are in modules with no `fn`. It passed on
  11 of 12 examples and on the task queue, as predicted.
- D3, wrong: the task queue has 86 mutants, not 350 to 700, and all 25 programs 633, not 1,500
  to 3,500. The plan's crude count of 650 counted places the table never changes.
- D4, wrong at the edge: 85 of 212 tested functions (40.1%) have no mutant, just above the 15% to
  40% predicted.
- D5, partly right: the Int literal row makes the most mutants (223); negate (130) comes before
  arithmetic (111); `not` makes the fewest (10).
- D6, wrong: the boundary row has the highest share of survivors (36%), then Int literals
  (28%); negate, logic and `not` are lowest, as predicted, but Bool is 11%.
- D7, right: no row is dropped (below).
- D8, wrong: the gated task queue took 17 minutes, under the 20 to 90 predicted; the other 24
  programs took 24 seconds together.

### The table of kinds

Decided by the agent under Robert's delegation of 2026-11-19: the table keeps all seven rows
unchanged, and the reference states it (section 13.4, rule 3). Reasons:

- I read all 70 survivors, not a sample: 62 from the library and examples and 8 from the task
  queue. Every one changes a result that some input reaches: a comparison at a limit
  (`limit < 1`, `ttl <= 0`, `attempts >= max_attempts`), an Int literal in a width, an exit
  status or an id, or a helper that only tests use. I judged each killable by a check. One, the
  task queue's `until > now` made `until >= now` in `Main.current?`, can only be killed by a call
  that arrives at the exact instant a lease ends, which takes one particular call sequence.
- The rows with the most survivors, boundary and Int, are the ones that catch off-by-one
  mistakes, the kind several wrong versions of the task queue make. Dropping them would remove
  the gate's reach where it matters.
- The logic row's 16 dropped mutants are `or` used as an Option default (`x or 0`) and `and`
  used with `is`; the checker refuses both swaps, so they cost one check each and never
  survive. That is not a reason to drop the row, which killed 20 others.
- A limit of this judgment: it reads the code and does not write the checks. PR4 fits these
  programs and will show whether each survivor can in fact be killed.
