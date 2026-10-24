# Benchmark v0, round 3: results

Forty fresh sessions, one per task per family, on mo 31c0f8c (`freeze.md`), 24 Oct 2026. The round measures Phase 2's first-retry targets again after MO-147 (#212) added a rule to the guide's loop: fix every error `mo check` reports before running it again (D296). The tally and the target are unchanged. Robert does not grade programs himself (D275), so the hidden tests decide each verdict, and the model graders' answers raise findings only.

## First retry

`python3 learnability/retry.py` over the forty logs, unchanged from rounds 1 and 2. The targets leave out E1103, E1150 and E1400; no session hit any of the three, so counting every code gives the same numbers.

| | Next attempt | 95% Wilson interval | Within three |
| --- | --- | --- | --- |
| Both families | 37 of 38, 97.4% | [86.5%, 99.5%] | 38 of 38, [90.8%, 100.0%] |
| Claude Code | 14 of 14, 100% | [78.5%, 100.0%] | 14 of 14 |
| GPT-6.1 Sol | 23 of 24, 95.8% | [79.8%, 99.3%] | 24 of 24 |

- **Next attempt: 97.4%, meeting the 85% target.** Even the interval's lower end, 86.5%, is above 85%. Round 1 measured 29 of 37 (78.4%, [62.8%, 88.6%]) and round 2 26 of 37 (70.3%, [54.2%, 82.5%]). Round 3's interval doesn't overlap round 2's.
- **Within three: 38 of 38**, a point-estimate pass of the 95% target, as in rounds 1 and 2.
- One `mo fix` ran (GPT-6.1 Sol's 07-grades) and changed nothing, so no error was moved to `by_fix`. Every session's last `mo test` runs passed, 603 of 603.
- The one error not fixed on the next attempt: **E0025** ("Mo writes or instead of ||"), in GPT-6.1 Sol's 07-grades, `grades.mo:28`. The session ran `mo fix grades.mo`, which had no fix to apply because E0025 offers none, then ran `mo check` again with the file unchanged. It replaced `||` with `or` before the following run.

## Edited before re-run (secondary)

D296 asks for this beside the tally, not as the item's measure. An error counts as edited when the session changed the code that fixes it before its next `mo check` or `mo test` run. Usually that is the reported line, but it can be the line the diagnostic's `Fix:` names (a `var` on the first binding for E0051), a new arm for an E0070 reported on the `case` line, or the `expose` line in the other file for an E0046. Each of the 13 errors whose own line didn't change was checked by hand against the session's snapshots.

| | Edited before the next run | Of those, gone on the next run |
| --- | --- | --- |
| Round 3 | 37 of 38 | 37 of 37 |
| Round 2 (`../bench-v0-round-2/results.md`) | 26 of 37 | 26 of 26 |

So the rise in the tally is the sessions fixing every reported error before re-running, as the new guide rule says. In round 2, GPT-6.1 Sol fixed one error and re-checked, or ran `mo test` before editing, in four sessions. In round 3 no session did either. The binary also changed between rounds (#202–#211, which, for example, reports 06-temperature's `to_text()` as E0095 instead of E0075). So the round cannot attribute the whole change to the guide rule, but the behaviour the rule asks for is what changed.

## Verdicts

39 of the 40 programs pass `bench/v0/grade.py`. The other, Claude Code's 02-col-sum, parses and checks, but fails the two blank-first-line runs added in round 2:

| Run | Expected | The program |
| --- | --- | --- |
| `14-blank-first-line`, input `\n1\n` | `: 1.00`, status 0 | `1: 0.00`, status 0 |
| `15-blank-only`, input `\n` | `: 0.00`, status 0 | `no header` on standard error, status 1 |

It takes the first non-blank line as the header, as all four 02-col-sum programs of rounds 1 and 2 did. GPT-6.1 Sol's program takes the first line, as the task says.

Both 11-calendar programs pass the new smallest-`Int` test (`freeze.md`).

95% Wilson intervals:

- All forty: 39 of 40, [87.1%, 99.6%].
- Claude Code: 19 of 20, [76.4%, 99.1%]. GPT-6.1 Sol: 20 of 20, [83.9%, 100.0%].
- Write a program, both families: 9 of 10, [59.6%, 98.2%]. Each other kind: 10 of 10, [72.2%, 100.0%].
- No task is held out (`freeze.md`), so no held-out result is reported.

## Model graders

Blind, with `grader-prompt.md`; identifiers in `grading-key.json`. Graders A (Claude Code) and B (GPT-6.1 Sol) both said yes to 39 programs and disagreed on one, which went to grader C (Claude Opus 5.5):

| Program | Grader A | Grader B | Grader C | Mechanical |
| --- | --- | --- | --- | --- |
| Claude Code 02-col-sum (qkurx3c) | yes | no | no | fail |

A called taking the first non-blank line as the header "a defensible reading". B and C said rule 1 makes the first line the header, and that blank-only input must not print `no header`. The hidden tests agree with B and C, so no conflict remained. This round, unlike round 2, the graders' final answer matches every mechanical verdict.

## Findings

1. **The first-retry item is met.** 37 of 38, 97.4%, [86.5%, 99.5%], on the unchanged tally. Under D296 and MO-148's acceptance, Phase 2 checklist item 9.1 can be ticked.
2. **E0025 has no `Fix:`.** The only miss came from a session that ran `mo fix` expecting it to replace `||`. Giving E0025 a fix (`||` to `or`, `&&` to `and`) would let `mo fix` apply it. This is a possible follow-up, not a change this round makes.
3. **02-col-sum's blank first line still catches a family.** One of the two programs fails it this round. Over three rounds, five of the six programs have skipped a blank first line. The task text could say outright that a blank first line is the header, as rounds 1 and 2 suggested.
4. **Grader A missed the 02-col-sum failure again.** B and C caught it this round. Agreement between graders still isn't evidence of correctness against a rule the hidden tests check.
