# The learnability protocol for Phase 2

How every Phase 2 measurement of fresh agent sessions writing Mo is run, graded and reported
(MO-65). It applies to:

- the three tools' diagnostic and final rounds (D230, `bench/tools/`);
- agent benchmark v0 (MO-62);
- the share of compile errors fixed on the first retry and within three, counted by the `tally`
  tool over those sessions' logs.

Phase 1's protocol (`round-1/`, `final/`) graded by reading, because there was no compiler.
Phase 2 has the `mo` command, so the compiler and hidden tests decide, and model graders only
check them. The rest of Phase 1's way of working stays: fresh sessions, one task each, two
model families (D226), frozen texts, and every program and log kept in the repository.

## 1. Freezing a round

Before the first session of a round starts, its folder holds `freeze.md`, which records:

- the `mo` commit the sessions use, and the SHA-256 of the binary built from it; that exact
  binary is copied to where the sessions run, and the grading uses it too;
- the SHA-256 of the reference (`docs/reference/README.md`) and the guide
  (`docs/agent-guide.md`) at that commit, which are what the sessions read;
- the prompt, word for word, with the place the task goes in marked;
- each task's text and hidden tests, by the hash in its `SHA256SUMS` (as `bench/tools/` does);
- the two families and the exact model and tool each session runs, such as Claude Code with its
  default model, and the date each model was released;
- the graders' models and their prompt. Phase 1's `rubric.md` assumes there is no compiler, so
  the first Phase 2 round writes a new grader prompt for section 4's question, and freezes it;
- the random seeds of section 5.

A round is frozen once `freeze.md` is committed. Changing anything it lists after a session has
seen a task makes a new round: the sessions already run are reported as their own round, never
mixed with the new one.

## 2. Sessions

- **Fresh.** A session has never seen Mo before: no memory, no earlier conversation, no project
  instructions. It is started for one task, and it ends when the task's files are written.
- **What a session gets.** The prompt with its task, the `mo` command built from the frozen
  commit, and an empty folder. It reads the reference and the guide through the command
  (`mo guide`, `mo guide --reference`). It has no checkout of this repository, no web access,
  and no other files. It may run `mo` as often as it likes.
- **Recording.** The harness, not the agent, records every `mo check`, `mo test` and `mo fix` the
  session runs, as the session log that `bench/tools/tally/task.md` defines (one numbered folder
  per run, holding `command`, the `--json` output, and a copy of `src/`). The agent sees the
  usual text output. `mo run` is recorded in the same log for `notes.md`, but `tally` doesn't
  count it, so an error seen only through `mo run` isn't in the first-retry measure. The harness
  is `learnability/harness/session.py`; its `README.md` says how a session is started through it
  and where `mo run` and the other commands are kept.
- **How many.** Each task is attempted by each family in a fixed number of sessions, set at
  freezing:
  - the three tools: one session per tool per family, six attempts in all (D229);
  - benchmark v0: one session per task per family, so at least 20 per family for 20 tasks
    (Robert, 18 October 2026).
- **Every session counts.** A session that ends without its files, runs out of time, or wanders
  off the task is graded as a fail; it is not run again. The one exception is a session that
  failed before it saw its task (the harness couldn't start it, or the model provider was down).
  That one is run again, and `notes.md` says why.

## 3. Grading: the compiler first

A program's verdict is mechanical:

| Step | Pass when |
| --- | --- |
| 1. Parses | the executable grammar (`grammar/mo.y`) accepts it, as in Phase 1 |
| 2. Checks | `mo check` reports no diagnostic |
| 3. Does the task | every hidden test passes, run by the round's grading script on the frozen `mo` |

A program **passes** when all three pass, and otherwise **fails** at the first step it doesn't
pass; the report counts fails by step. A program whose only diagnostics are the ones the guide
tells sessions to keep (E1103 or E1150 on a form the reference requires, E1400 from a promise
`mo test` can't build inputs for) is reported as a **toolchain gap**, apart from passes and
fails, since the toolchain stopped it and not the session. Its hidden tests still run where the
gap allows. Phase 1's "close" (at most two mistakes a compiler would
catch) has no Phase 2 counterpart: a session now has the compiler, and a program that doesn't
check can't run its tests.

For the three tools, `bench/tools/grade.py` is the grading script, and for benchmark v0's tasks,
`bench/v0/grade.py` (`bench/v0/README.md`). For the benchmark, every task has hidden tests,
whatever its kind:

- **write a program:** tests of its behaviour, as the tools have;
- **fix compile errors:** `mo check` is clean and the program's original tests still pass;
- **make a failing test pass:** the named test passes and every other test still passes, run
  from the hidden copy of the tests, so editing or deleting a test doesn't help;
- **extend a program:** tests of the new behaviour and of the old.

A task is frozen only when its reference solution, written by the task's author, checks with no
diagnostic and passes all of its hidden tests on the frozen `mo`, and a deliberately broken
solution fails at least one. So no task needs an unfinished form; `bench/tools/README.md` holds
back the tools' rounds for the same reason.

## 4. Model graders check the tests

Hidden tests can be wrong both ways: a program can print the expected output for the test inputs
without doing the task, and a test can fail a program that does it. So two model graders read
every program that reaches step 3 and answer one question: does it do what the task text asks,
yes or no? Afterwards they write the stumbles list, as Phase 1's rubric describes, from the
diagnostics `mo check` gave in the session log.

- **One Claude grader and one non-Claude grader.** Claude grading Claude invites
  self-preference, so the second grader is from the other family (GPT-6.1 Sol today). Neither
  wrote the reference, the tasks, or any attempt.
- **Blind.** Each program is renamed to a random identifier before grading, and anything naming
  its family, model or session (file names, comments that say so) is removed. Each grader sees
  the programs in its own random order, with the task text and the reference solution. It does
  not see the hidden tests' results, or which step a program failed at, until it has answered,
  so the tests can't sway its answer.
- **On its own.** Each grader grades every program alone. When the two disagree with each other,
  or with the mechanical verdict, the program goes to a third fresh grader, and then to Robert
  if that still leaves the graders and the tests in conflict.

When the graders' answer differs from the hidden tests' and the third grader or Robert agrees
with the graders, that is a finding against the hidden tests, not a new verdict: the program
keeps its mechanical verdict for this round, pass or fail, the finding is written in
`results.md` beside it, and the test is fixed for the next round, which is a new freeze.

## 5. The human check

Before the model graders' answers are relied on, Robert grades a sample of programs himself.

- **Which programs.** 30 programs that the model graders also read, drawn at random with the
  seed recorded in `freeze.md`. A round with fewer than 30 is graded whole, and the next rounds
  add to it until 30 are graded; until then the graders' answers only raise findings.
- **How.** Blind, as section 4 describes, with the same question, and without the hidden tests'
  results: does it do the task, yes or no.
- **What's computed.** Cohen's kappa between Robert and each model grader, and between Robert and
  the mechanical verdict, with the observed agreement beside each
  (`python3 learnability/wilson.py --kappa FILE`).
- **What it means.** A grader (or the hidden tests) agrees with Robert well enough when its kappa
  is at least 0.6. Kappa is unstable when one answer is rare: with 29 of 30 programs passing,
  one disagreement gives a kappa of 0. So when Robert gave either answer to fewer than 5
  programs, agreement is judged instead by at most 3 disagreements in 30 (90% agreement). On
  30 programs a kappa is only known to about 0.15 either side, so the rule is a floor, not a
  fine measure.
- **What follows.** Every disagreement with Robert is looked at and written down, whatever the
  numbers. A model grader that falls short isn't used in the round. If the hidden tests fall
  short, they are not trusted: each disagreement is traced to a test or to Robert's reading
  before any result is reported.

The check is done by the end of benchmark v0's round at the latest, and again whenever a
grader's model or prompt changes.

## 6. Held-out tasks

- Benchmark v0 holds out at least 4 of its tasks (D229). A held-out task is written for this
  round, after the release date of every model taking part, and no session, grader, or published
  document has seen it before the round starts.
- Held-out tasks are committed with their hashes at freezing, like the rest. The sessions have no
  access to the repository or the web, so committing them doesn't show them to a session.
- After the round, its held-out tasks are public and become ordinary tasks. The next round writes
  new held-out tasks.
- Results are reported separately for held-out and other tasks. With 4 held-out programs per
  family the held-out rate is very rough (2 of 4 is [15.0%, 85.0%]), so the two families' held-out
  programs are also reported together. A gap counts only when the two Wilson intervals don't
  overlap, and then it means the sessions learned the samples and the guide's examples rather
  than the language; a smaller gap is reported as a hint, not a finding.

## 7. Uncertainty: the Wilson interval

Every rate is reported as K of N with its 95% Wilson interval
(`python3 learnability/wilson.py K N --target T`). Against a target:

- **pass:** the interval's lower bound is at or above the target;
- **point-estimate pass:** the rate meets the target but the lower bound doesn't, so the result
  could be chance;
- **miss:** the rate is below the target.

What each measurement can show:

| Measurement | Target | N | Best possible result |
| --- | --- | --- | --- |
| Three tools, final round | at least 5 of 6 attempts pass (D229) | 6 | point-estimate pass: even 6 of 6 has a lower bound of 61.0% |
| First retry | at least 85% of errors fixed by the next run | errors in the final round and benchmark sessions | a pass needs 22 errors all fixed, 34 with 33 fixed, or 45 with 43 fixed; at a 90% rate, about 200 errors |
| Within three | at least 95% fixed within three runs | the same errors | a pass needs 73 errors all fixed, or 110 with 109 fixed (99%) |
| Benchmark v0 | none; it is the baseline later phases are compared with | 20 per family for 20 tasks | 17 of 20 (85%) gives [64.0%, 94.8%]; a baseline this wide shows only large changes in later phases |

The tools' target is D229's and stays as written: 5 of 6 meets it, and the report says it is a
point-estimate pass with its interval.

`wilson.py --size P W` gives how many attempts a new measurement needs for an interval of W
either side of a rate P.

The interval assumes each attempt is independent, and these aren't quite:

- **Errors.** One session can make the same kind of mistake many times. So beside each
  first-retry rate the report gives the number of sessions and how many of the errors came from
  the session with the most, and repeats the rate with that session left out.
- **Benchmark programs.** The two families' sessions on one task share its difficulty. So the
  report also gives each task's result for both families, and says how many tasks both passed
  and how many neither did.

In both cases the true interval is wider than the one printed, and the report says so when one
session or task dominates.

Errors the guide tells sessions to keep (E1103, E1150, and E1400 from `mo test`) aren't the
session's to fix. `tally` counts them, since its task is frozen, so the report gives the rates
twice: as `tally` prints them, and with those codes left out of the errors. The second is the one
measured against the targets. `tally` moves an error a `mo fix` run precedes to `by_fix`. Before
MO-48 that run did nothing (E1400), so a round on such a build says how many errors that moved.

`python3 learnability/retry.py LOGS` prints all of this for a round's logs: `tally`'s table from
the reference (`bench/tools/tally/hidden/reference.py`), then for both sets of errors the two
rates with their Wilson intervals, the errors `mo fix` moved, the number of sessions, the errors
from the session with the most, and the rates without that session.

## 8. Reporting

Each round's folder (as `final/` is for Phase 1) holds `freeze.md`, the prompt, every program as
written, every session log, every grader's grading, the human check's grades, `notes.md` (how
the round ran, and any session run again and why), and `results.md`, which gives, per family:

- pass counts, fail counts by step and toolchain gaps, by task kind and for held-out and other
  tasks apart, each with its Wilson interval, and each task's result;
- the `tally` table and its rates, with `mo fix` counted separately, and the tests' pass rate
  beside it, because "it compiles" is not "it works";
- the human check's kappas and agreement;
- every finding against the hidden tests, and the stumbles.
