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
