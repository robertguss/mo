# Trust gates: the dry run, the measurement and the full run

The record of PR3 and PR4 of `docs/plans/2026-11-18-2013-feat-trust-gates-mo-test-plan.md`: U8,
the dry run that settles the table of kinds; U10, the measurement of how many of the 24 wrong
versions a task queue fitted only to the gates kills; and U11 and U12, the repository's programs
fitted to the gates and the cost of their full run. The result files are in
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

The table was frozen at commit `5cabd4ff` (`src/tools/mutants.c` blob `bcb7a089`). Every run in
the measurement below used a build of that commit's code.

## The measurement (U10)

### What was measured

The task queue as committed at `1389bdfb`, before its intent was strengthened: `main.mo`,
`job-queue.mo` and `queue-tests.mo`. `learnability/trust-gates/manifest.json` holds their hashes,
written before the first run; `main.mo` and `job-queue.mo` match the Phase 6 manifest, and the
copies measured and handed to the fitting session were checked against it. Every run goes
through `queue-tests.mo` (KTD16), against the working half
(`learnability/phase6/strength/working`) and the held-back folder the Phase 6 manifest names.
Every `--strength` run used `--seconds 360`. The slowest check is `QueueTests.test.22`: 71.4
seconds on the frozen program and 64.1 on the fitted one, so 360 is 5.0 and 5.6 times it. No
run was cut short (`partial` is false in all four records).

### The fitting session

- **Where:** a new folder outside the repository, `/home/exedev/tg-fit-20261119`, holding
  `toolchain/mo` (the debug gcc build of `5cabd4ff`, built from a source export inside that
  folder so its library path points there, after which everything but the binary and `lib/`
  was deleted), `toolchain/lib/` (the 12 library modules), `guide.txt` (what `mo guide`
  prints), `frozen/` (the three files, read-only) and `work/` (a copy to change). No wrong
  version, result, record or other repository file was in it.
- **Who:** a fresh `claude -p` session (Claude Code 2.1.327, model `claude-opus-5-5`) started
  there with `--setting-sources project`, `--strict-mcp-config` (no MCP servers) and a fresh
  auto-memory folder. Its tools were Bash, Read, Edit, Write, NotebookEdit, ToolSearch,
  TaskStop and ReportFindings; subagents, web search and fetch, skills, artifacts, messaging,
  scheduling and monitoring were disallowed. Its permission mode was bypass, so it could run
  any command.
- **Isolation is by instruction, not enforced.** Its brief (`transcript/brief.md`) told it to
  read, list and run only what is inside the folder. Nothing stopped its shell from reading
  any path my user can read, the repository and the held-back folder included. I audited the
  transcript afterwards: its 18 tool calls (17 Bash, 1 Read) name no path outside the folder
  and no `..`. The brief also told it to keep every declaration's name and signature and never
  to delete or weaken a check, which keeps the wrong versions countable; the transcript shows
  it did.
- **What it did,** in 27 minutes and 19 turns: the first full run failed stated intent for six
  tools, so it wrote 12 examples on them in `main.mo`. The next full run left 6 survivors; it
  wrote 4 examples on `JobQueue.submit` and `JobQueue.lease` and rewrote the test helper
  `QueueTests.job`, whose `.jobs.first` made a page limit of 1 and 2 indistinguishable. The
  last two full runs passed: 65 mutants tried, 65 killed (1 by a promise, 35 by examples, 19 by
  tests, 10 at start), 1 dropped. My own two gated runs of the fitted copy also exit 0, with
  identical mutant records. Its log (`transcript/log.md`) counts 1 survivor it judged no check
  could kill in its original form (the helper's limit) and 0 that only one particular call
  sequence killed.

### The results

| Program | Half | By promise or example | By a test only | By any check | Survived | Uncountable |
| --- | --- | ---: | ---: | ---: | --- | ---: |
| frozen | working | 3 | 6 | 9 of 12 | w02, w03, w09 | 0 |
| fitted | working | 4 | 5 | 9 of 12 | w02, w03, w09 | 0 |
| frozen | held-back | 2 | 8 | 10 of 12 | w16, w18 | 0 |
| fitted | held-back | 5 | 5 | 10 of 12 | w16, w18 | 0 |

Fitting the task queue to the gates killed no wrong version that the frozen program's own
checks did not already kill: 19 of 24 before and 19 of 24 after, with the same five survivors.
What changed is which check kills. Four versions that only a test killed (w01, w15, w19, w22)
are now killed by an example the fitting wrote, and two that a promise killed (w06, w13) by an
example as well, so the promise-and-example rate rose from 3 to 4 of 12 on the working half and
from 2 to 5 of 12 on the held-back half.

The three working-half survivors are mistakes that take several steps to show: w02 counts the
5,000-job limit over Ready and Leased jobs only, w03 gives back only the last of several leases
that expire in one tick, and w09 turns `fail` on a lease that is not current into a silent
success. `Main.fail`, which w09 changes, has no mutant at all, and the 4 mutants of
`Main.submit` and the 3 of `JobQueue.expire`, which w02 and w03 change, were all killed in the
session's first mutant run, so the gate never asked for a check of those paths. Phase 6's hand-strengthened intent killed w02,
w03 and w09; the gate-fitted program does not.

| | Frozen | Fitted |
| --- | ---: | ---: |
| Mutants (boundary, negate, logic, not, arithmetic, bool, int) | 67 (11, 18, 7, 0, 6, 1, 24) | 66 (11, 18, 6, 0, 6, 1, 24) |
| Tested functions | 35 | 35 |
| Functions with no mutant | 17 | 17 |
| Functions that lost all their mutants | | none |

The one mutant fewer is the `or` the helper rewrite removed from `QueueTests.job`.

### The numbers of R24

- **Full run, task queue:** 1,015 seconds for the current `services/task-queue` (86 mutants,
  10 service promises), from the dry run; 222 seconds for the fitted program (66 mutants, 4
  service promises) on an idle machine, and 228 beside two `--strength` runs. Plain `mo test`
  takes 75.5 seconds on the frozen program and 79.3 on the current one.
- **Full run, library and examples:** 7.0 and 17.3 seconds, from the dry run.
- **Shorter run:** 160.5 seconds for the fitted program `--against` an unchanged copy of
  itself (18 functions carried, no mutant tried; the time is both versions' checks). Against
  the frozen copy it carried nothing and took 306 seconds, because the fitting changed
  `QueueTests.job`, a helper every simulation test reads, so every function's checks changed
  (KTD11).
- **Mutants nothing could kill:** 0 in the fitted program; the fitting session judged 1 unkillable
  in its original form and removed the reason with a rewrite.
- **Survivors only a rare call sequence could kill:** 0 in the fitting log. In the dry run I
  judged one of 70 to be of that kind (`Main.current?`, `until > now`).

### The predictions against the measurement

- P1, partly wrong: the frozen rate by promise or example was 3 of 12 on the working half, the
  same three versions as Phase 6's `1-before.json`, as predicted. By any check it was 9 of 12,
  not about 5 (4 to 7), and 10 of 12 on the held-back half, not about 5 (3 to 7). I
  underestimated the 23 tests.
- P2, half right: the session made the gates pass, but in 27 minutes, not 2 to 5 hours.
- P3, right in number and wrong in kind: 9 of 12 and 10 of 12 by any check are inside the
  ranges, but none of it is new; the predicted new kills from examples on the tools did not
  happen.
- P4, right: no version became uncountable.
- P5, right: one mutant fewer, the same functions with no mutant, none lost.
- P6, half right: 1 judged unkillable (0 to 3 predicted); 0 killed only by one call sequence
  (1 to 5 predicted).

### The recheck after the code review

The final code review found that a wrong version on which a promise or an example stops and a
test fails was `killed_by_test`, so scored, where before tests ran it was `defect` and kept out
of the rate. The tool that made the four `--strength` runs above had this fault. A version now
stays `defect` when a promise or an example stopped (`e-stop.mo` in
`tests/cli/strength-tests/versions`). All four runs were made again with the fixed tool, into
`learnability/trust-gates/results/strength/recheck/`.

Prediction, written at 06:34 UTC on 2026-11-19, after the four runs started and before any of
their output was read: every version keeps the outcome it had above, because each
`killed_by_test` version above was killed by a test failing on a wrong result, and I know of
none on which a promise or an example stops.

Result: right. All 48 outcomes are the same in the four rechecks as in the runs above, with the
same counts (`killed`, `killed_any`, `scored`, `partial`) and an empty stderr; the runs took
1,146 to 1,254 seconds (`recheck/runs.tsv`). The tool was the debug build of `330b1666`. The
numbers in this record therefore stand as written.

### Limits

- The held-back half was already used in Phase 6, against the strengthened program
  (`learnability/phase6/strength/results/5-held-back.json`).
- No wrong version replaces `JobQueue.submit`, `lease`, `acknowledge` or `fail` (checked over
  both halves), so this says nothing about how the gate covers those operations.
- The count measures both gates together: six of the seven tools had no stated intent, and
  the examples written for them are part of what was fitted.
- The table's author had read what the wrong versions changed, in Phase 6 and before freezing
  the table.
- `--strength` runs each service check for 1,000 call sequences; the gate runs 200.
- The fitting session's isolation is by instruction (above). It was also told it was in an
  experiment whose integrity depended on the rule, and to keep names and signatures; a session
  working on its own might restructure code and make wrong versions uncountable.
- One fitting session, one program, 24 wrong versions: the numbers show what happened once,
  not a rate to expect.
- The predictions were written after I had read the Phase 6 results, the wrong versions'
  mistakes in its manifest and the programs' sources.
- Wall times are from one shared 8-core machine, several runs at a time, the debug build.
- Someone other than the fitting session counted again: a fresh agent given only the six
  result files (`transcript/recount.md`). Its counts agree with every number above.

### The answer

Decided by the agent under Robert's delegation of 2026-11-19, in place of Robert's answer:
continue with PR4 and PR5 as planned, keep the gates behind `--gates` until R30 is measured,
and say plainly what this measurement shows. Reasons:

- On this program, passing the gates added no kill of a mistake an agent thought plausible.
  The frozen task queue's tests already killed 19 of 24, and the five left are multi-step
  mistakes that no one-token mutant stands for. A passing gate is therefore a floor on how
  thoroughly a program's checks exercise its code, not evidence that they catch the mistakes
  agents make, and the guide must not claim more.
- The gate still did work an agent would otherwise skip: it found six tools with no stated
  intent and six places where no check noticed a change, and it moved four kills from tests to
  examples, which state intent where a reader looks for it. That is consistent with the Goal
  Capsule's first half, a public surface that states what it is for.
- R25: no number decides whether the gate is kept. The measured costs (under 4 minutes for the
  fitted task queue, 24 seconds for the library and examples, but 17 minutes for the current
  task queue with 10 service promises) are what R30 and U12 must weigh.

## The repository's programs and their full run (PR4)

### Fitting (U11)

On 2026-11-19, from `main` at `82b7df1f`, every program of R29 was made to pass
`mo test --gates`: the 12 library modules, the 12 examples and
`services/task-queue/queue-tests.mo`. `learnability/trust-gates/fitting-log.md` is the log: each
program's survivors before and after, its mutant counts, the survivors no check could kill and
how each survivor was killed.

- 104 survivors and 4 functions with no stated intent before; none after. The dry run's 70
  survivors are among them; the other 34 are in `lib/json.mo` and `examples/account.mo`, whose
  mutants were not tried until they stated intent.
- Mutants: 633 before, 616 after. Functions with no mutant: 85 of 212 before, 86 of 211 after.
  Two functions lost all their mutants: `Inventory.pad_column` and `RateLimit.limiter_of`.
- Nine survivors could not be killed by any check of the program as written, because they
  change nothing a check can observe; each was resolved by simpler code. Three more in
  `lib/json.mo` only send a string down the parser's slow walk; a test kills them by the gate's
  step limit (the log gives the evidence), and the parser keeps its speed: 28,140,943
  instructions on Phase 7's 1 MiB body against 28,842,519 on `main`.
- Survivors that only one call sequence could kill: none. The one the dry run judged of that
  kind, `Main.current?`, is killed by an example on that helper.
- Every new check kills at least one mutant that survived, or states the intent of an exposed
  function that had none.

### The full run and its cost (U12)

`ci/check_gates.py` runs `mo test --gates` on the 25 programs with the first driver, at most as
many at a time as there are processors, prints each one's time and fails on any program that
does not pass. It then removes the test "a total follows two or more files…" from a copy of
`examples/wc.mo` and must see that copy fail with survivors. It runs in the `drivers` group,
which the nightly runs.

Measured on 2026-11-19 on this host (8 cores, shared, the debug gcc build), one run each, with
nothing else running unless said:

| What | Seconds |
| --- | ---: |
| Full run, task queue | 198.1 |
| Full run, 12 library modules, one after another | 21.8 (`lib/json.mo` 16.6) |
| Full run, 12 examples, one after another | 18.1 (`examples/json-query.mo` 5.3) |
| `ci/check_gates.py`, all 25 and the control | 200.9 |
| Shorter run, task queue, `JobQueue.lease` changed | 189.8 (5 mutants tried, 23 functions carried) |
| Shorter run, `lib/json.mo`, `Json.hex_digit_value` changed | 4.3 (9 tried, 26 carried) |
| Shorter run, `examples/json-query.mo`, `JsonQuery.hex_digit_value` changed | 0.8 (9 tried, 14 carried) |
| `bash ci/check.sh drivers` on `main` (`82b7df1f`), one driver | 572 |
| `bash ci/check.sh drivers` on this branch, one driver | 628 |
| Plain `mo test`, task queue, for comparison | about 75 |

The task queue's full run fell from 933 seconds before fitting to 198 after. A survivor costs
the most: no check kills it, so every check that entered its function runs to the end, and the
task queue's full-queue test alone takes about 70 seconds. Once every mutant dies, most die on
the first cheap check. The task queue's shorter run takes about as long as its full run,
because it runs every check on both versions first.

### R30

Decided by the agent under Robert's delegation of 2026-11-19: the full run meets R30, measured
here, for PR5 to turn the gates on. Reasons:

- The full run of all 25 programs takes about 3.5 minutes of wall time on one driver, and it
  runs beside the other checks of the `drivers` group, so the group grew by 56 seconds (10%)
  here. Its longest check is still the formatter's, at about 450 seconds; the gates' check took
  238 inside the group. One run each, on a shared host whose load average was 3.6 when the
  branch's run started and 1.6 when `main`'s did. The nightly of 18 November
  took 77 of its 120 minutes, about 40 free, and runs the gates' check once, on its first
  driver. Its runner is not this host; the first nightly on `main` after this merges gives that
  time.
- PR5 cannot turn the gates on by making every plain `mo test` in the drivers a full run: the
  task queue's checks in `ci/check_task_queue.py` and elsewhere would each take over 3 minutes
  on the debug build and several times that under the sanitizers, on three drivers. U14 already
  plans that checks whose claim is about something else select with `--in`; PR5 must do that for
  every plain `mo test` of the task queue, and measure the nightly again.
- An agent working on the task queue gets no speed from `--against`, since the checks on both
  versions take as long as the mutants. That is a cost of the shorter run, not of R30, and the
  first lever is the one the plan names: a cheaper test of the full queue.

## Decided under delegation

Each of these was decided by the agent under Robert's delegation of 2026-11-19.

- **The table of kinds** keeps all seven rows, is stated in the reference (13.4, rule 3) and
  is frozen at `5cabd4ff` (above).
- **The answer** in U10 step 7 (above).
- **An instrument the plan did not ask for.** U8 needs mutants by row and by function, which
  the gate did not report. `mo test --gates --list` now lists each mutant and each function's
  count, and a gated `--json` run writes a `mutant` record for each mutant with its outcome,
  kind of check and check. The plan's U3 refused `--gates` with `--list`; that refusal is
  lifted. Text output of a gated run is unchanged.
- **What `killed_by_test` covers.** KTD17 names a kill by a test; R23 says any check may kill.
  A recovery check's kill also counts as `killed_by_test`, and so does a test or recovery check
  that stops on a wrong version, as the gate counts a stopped check (found by the code review,
  fixed in `ea0b1c19`). A check that did not finish still makes a version `unfinished`, and a
  promise or an example that stopped still makes it `defect` (found by the final code review;
  the recheck above).
- **A defect on `main` fixed here:** the intent walk's crash on recursive types (`04a62857`),
  because it blocked U8's listings.
- **`--seconds 360`** for every `--strength` run, at least five times the slowest check.
- **The fitting session's setup:** `claude -p` on `claude-opus-5-5`, the tools and folder
  described above, and a brief that kept names and signatures and forbade weakening a check.
  It rewrote a test helper rather than adding a check or simplifying code; I accepted that as
  within the brief's intent, since no check was weakened.
- **The predictions** were mine, not Robert's.

In PR4:

- **`lib/json.mo`'s fast paths stay.** The helper fitting it first deleted three fast paths,
  because their mutants change only speed. That cost 72% more instructions on Phase 7's 1 MiB
  body, past the 2-second bar, so the change was undone. The plan prefers a new check to a code
  change in the four library modules compiled into the toolchain, and a test of one long plain
  string kills those mutants by the gate's step limit. A test that kills by cost is weaker
  evidence than one that kills by result: if the step multiple rises or the slow walk gets
  cheaper, those three mutants survive again and the gate says so.
- **Simpler code for survivors no check could kill** (R7): `Json.skip_space` compares Chars
  instead of codes, `Json.plain` pads once instead of branching three ways and drops a redundant
  guard, `Expenses.layout` and `Inventory.pad_column` read their widths with a `case`,
  `RateLimit.limiter_of` is deleted and `QueueTests.job` takes its page's only job. Each behaves
  the same for every input.
- **Examples on the task queue's promise helpers.** `Main.current?` and `Main.matching` are
  read only by the service promises. Examples on them kill their survivors without a service
  sequence.
- **The sample golden cases' expected output** for `mo test` was taken from runs, and each new
  line was checked against the check added for it; the totals rise by exactly those checks.
- **What `ci/cli_gates.py` pins on the library.** `lib/list.mo` now passes the gate, and no
  library module lacks stated intent, so the check that `Json.to_text@JsonNumber` fails
  stated intent is now a shorter run of `lib/json.mo` that passes it. The fixtures under
  `tests/cli/gates/` still show E1440 failing.
- **The door's proof keeps its frozen program.** The plan says to stop when a file a freeze
  hashes would have to change. The planted set of `learnability/phase6/door/planted/` hashes the
  task queue's `main.mo` and `job-queue.mo`, and fitting the task queue had to change both: no
  check outside them can reach the helpers and promise helpers whose mutants survived. The
  planted files replace whole files of that program, so with the fitted program the door read
  two planted fixes and a wrong version as intent changes. Rather than change the frozen set,
  `ci/check_door.py` now serves `learnability/phase6/door/program/`, the two files at the hashes
  the manifest names (from `82b7df1f`), and its proof is unchanged: on this branch it passes, and
  the bypassed build still fails with exactly its 7 expected mismatches.
- **R30**, above.
- **Phase 7's M3 waits for PR5**, the plan's default: until the gates are the default, an agent
  writing the client must remember `--gates`.
- **`tools/*.mo` are not fitted here**, the plan's default.
