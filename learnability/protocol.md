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

- the `mo` commit the sessions use, and the SHA-256 of the binary built from it;
- the SHA-256 of the reference (`docs/reference/README.md`) and the guide
  (`docs/agent-guide.md`) at that commit, which are what the sessions read;
- the prompt, word for word, with the place the task goes in marked;
- each task's text and hidden tests, by the hash in its `SHA256SUMS` (as `bench/tools/` does);
- the two families and the exact model and tool each session runs, such as Claude Code with its
  default model, and the date each model was released;
- the graders' models and their prompt;
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
  usual text output.
- **How many.** Each task is attempted by each family in a fixed number of sessions, set at
  freezing:
  - the three tools: one session per tool per family, six attempts in all (D229);
  - benchmark v0: three sessions per task per family, so at least 60 per family for 20 tasks.
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
pass; the report counts fails by step. Phase 1's "close" (at most two mistakes a compiler would
catch) has no Phase 2 counterpart: a session now has the compiler, and a program that doesn't
check can't run its tests.

For the three tools, `bench/tools/grade.py` is the grading script. For the benchmark, every task
has hidden tests, whatever its kind:

- **write a program:** tests of its behaviour, as the tools have;
- **fix compile errors:** `mo check` is clean and the program's original tests still pass;
- **make a failing test pass:** the named test passes and every other test still passes, run
  from the hidden copy of the tests, so editing or deleting a test doesn't help;
- **extend a program:** tests of the new behaviour and of the old.

A task is frozen only when its reference solution, written by the task's author, passes all of
its hidden tests on the frozen `mo` and a deliberately broken solution fails at least one.

## 4. Model graders check the tests

Hidden tests can miss things: a program can print the expected output for the test inputs
without doing the task. So two model graders read every program that passes, and every program
that fails at step 3, and answer one question: does it do what the task text asks? They also
write the stumbles list, as Phase 1's rubric describes, from the diagnostics `mo check` gave.

- **One Claude grader and one non-Claude grader.** Claude grading Claude invites
  self-preference, so the second grader is from the other family (GPT-6.1 Sol today). Neither
  wrote the reference, the tasks, or any attempt.
- **Blind.** Each program is renamed to a random identifier before grading, and anything naming
  its family, model or session (file names, comments that say so) is removed. Each grader sees
  the programs in its own random order, with the task text, the reference solution, and the
  hidden tests' results.
- **On its own.** Each grader grades every program alone. When the two disagree with each other,
  or with the mechanical verdict, the program goes to a third fresh grader, and then to Robert
  if that still leaves the graders and the tests in conflict.

A grader's "doesn't do the task" on a program the tests pass is a finding against the hidden
tests, not a verdict: the program stays a pass for this round, the finding is written in
`results.md`, and the test is fixed for the next round, which is a new freeze.

## 5. The human check

Before the model graders' answers are relied on, Robert grades a sample of programs himself.

- **Which programs.** 30 programs drawn at random from the round, with the seed recorded in
  `freeze.md`. A round with fewer than 30 programs is graded whole.
- **How.** Blind, as section 4 describes, with the same question: does it do the task, yes or no.
- **What's computed.** Cohen's kappa between Robert and each model grader, and between Robert and
  the mechanical verdict, with the observed agreement beside each
  (`python3 learnability/wilson.py --kappa FILE`).
- **What it means.** A kappa of at least 0.6 means a grader agrees with Robert well enough to be
  used. Below 0.6, that grader's answers are not used in the round, and each program it
  disagreed with Robert on is looked at. If the mechanical verdict is below 0.6, the hidden tests
  are not trusted: each disagreement is traced to a test or to Robert's reading before any
  result is reported. Kappa can be low, or undefined, when almost every program passes, so the
  observed agreement is always reported with it.

The check runs in the first round that uses model graders, and again whenever a grader's model
or prompt changes.

## 6. Held-out tasks

- Benchmark v0 holds out at least 4 of its tasks (D229). A held-out task is written for this
  round, after the release date of every model taking part, and no session, grader, or published
  document has seen it before the round starts.
- Held-out tasks are committed with their hashes at freezing, like the rest. The sessions have no
  access to the repository or the web, so committing them doesn't show them to a session.
- After the round, its held-out tasks are public and become ordinary tasks. The next round writes
  new held-out tasks.
- Results are reported separately for held-out and other tasks. A large gap between them means
  the sessions learned the samples and the guide's examples rather than the language.

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
| First retry | at least 85% of errors fixed by the next run | errors in the final round and benchmark sessions | a pass needs 45 errors with 43 fixed (95%), or about 180 at 90% |
| Within three | at least 95% fixed within three runs | the same errors | a pass needs 73 errors all fixed, or 110 with 109 fixed (99%) |
| Benchmark v0 | none; it is the baseline later phases are compared with | 60 per family for 20 tasks | about 9 points either side of an 85% rate |

The tools' target is D229's and stays as written: 5 of 6 meets it, and the report says it is a
point-estimate pass with its interval.

Errors in the first-retry measure aren't independent: one session can make the same kind of
mistake many times. So beside each rate the report gives the number of sessions, and how many of
the errors came from the session with the most, and it repeats the rate with that session left
out. The interval assumes independence, so when one session dominates, the interval is narrower
than the truth and the report says so.

## 8. Reporting

Each round's folder (as `final/` is for Phase 1) holds `freeze.md`, the prompt, every program as
written, every session log, every grader's grading, the human check's grades, `notes.md` (how
the round ran, and any session run again and why), and `results.md`, which gives, per family:

- pass counts and fail counts by step, by task kind, and for held-out and other tasks apart, each with
  its Wilson interval;
- the `tally` table and its rates, with `mo fix` counted separately, and the tests' pass rate
  beside it, because "it compiles" is not "it works";
- the human check's kappas and agreement;
- every finding against the hidden tests, and the stumbles.
