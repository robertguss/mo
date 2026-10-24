# Benchmark v0, round 2: results

Forty fresh sessions, one per task per family, on mo c7ce9c9 (`freeze.md`), 24 Oct 2026. The round re-measures Phase 2's first-retry targets after MO-137 (#201) changed the E0051, E0075 and E0050 diagnostics. Robert does not grade programs himself (D275), so the hidden tests decide each verdict, and the model graders' answers raise findings only.

## First retry

`python3 learnability/retry.py` over the forty logs. The targets leave out E1103, E1150 and E1400; no session hit any of the three, so counting every code gives the same numbers.

| | Next attempt | 95% Wilson interval | Within three |
| --- | --- | --- | --- |
| Both families | 26 of 37, 70.3% | [54.2%, 82.5%] | 37 of 37, [90.6%, 100.0%] |
| Claude Code | 15 of 15, 100% | [79.6%, 100.0%] | 15 of 15 |
| GPT-6.1 Sol | 11 of 22, 50.0% | [30.7%, 69.3%] | 22 of 22 |

- **Next attempt: 70.3%, missing the 85% target.** Round 1 measured 29 of 37, 78.4%, [62.8%, 88.6%]. The two intervals overlap almost entirely, so the round shows no change either way.
- **Within three: 37 of 37**, a point-estimate pass of the 95% target, as in round 1.
- No `mo fix` ran. Every session's last `mo test` runs passed, 623 of 623.

The eleven errors not fixed on the next attempt were all GPT-6.1 Sol's:

| Session | Errors not fixed on the next run | What the session did before that run |
| --- | --- | --- |
| 06-temperature | E0070, E0075, E0051 | fixed the E0050 on lines 37–38 only, then ran `mo check` again |
| 08-playlist | E0024, E0006 | edited nothing, and ran `mo test` |
| 09-rpn | E0068, E0077 | fixed the E0053 and E0050 on lines 25 and 44 only, then ran `mo check` again |
| 10-restock | E0086, E0056, E0070, E0053 | edited nothing, and ran `mo test` |

By code: E0070 twice, and E0006, E0024, E0051, E0053, E0056, E0068, E0075, E0077 and E0086 once each. Every one was fixed on the session's following run.

## What the miss measures

The tally counts an error as fixed on the next attempt when it is gone from the next `mo check` or `mo test` run, whether or not the session edited it in between. In this round, every error a session edited before its next run was gone on that run: Claude Code's 15 and GPT-6.1 Sol's 11, 26 of 26. All eleven misses are errors the session had not touched yet. It fixed one error and re-checked before fixing the rest, or it ran the tests before editing at all.

Round 1's eight misses follow the same pattern. All were GPT-6.1 Sol's, in 06-temperature (two left after it fixed lines 37–38 and 55), 09-rpn (two left after it fixed lines 25 and 44) and 07-grades (four, where it ran `mo test` without editing). So in both rounds, the gap to 85% comes from how often GPT-6.1 Sol runs `mo` between edits, not from a diagnostic it misread. MO-137's three codes support this: E0050 was fixed on the next run 7 of 7 times, and the one E0051 and one E0075 missed were both in 06-temperature, untouched before the next run.

So the 85% target cannot be met by better diagnostics alone while the measure counts errors a session has not yet reached. Whether to change the measure (count an error only after a run in which its line was edited), the guide's advice on fixing every reported error before re-running, or the target is a decision for Robert. This round changes none of them.

## Verdicts

38 of the 40 programs pass `bench/v0/grade.py`. The other two, both families' 02-col-sum, parse and check, but fail the two runs added for this round (step 3):

| Run | Expected | Both programs |
| --- | --- | --- |
| `14-blank-first-line`, input `\n1\n` | `: 1.00`, status 0 | `1: 0.00`, status 0 |
| `15-blank-only`, input `\n` | `: 0.00`, status 0 | `no header` on standard error, status 1 |

Both skip blank lines before taking the header, as both of round 1's programs did. Round 1's finding 1 recurs, and the hidden tests now catch it.

95% Wilson intervals:

- All forty: 38 of 40, [83.5%, 98.6%].
- Each family: 19 of 20, [76.4%, 99.1%].
- Write a program, both families: 8 of 10, [49.0%, 94.3%]. Each other kind: 10 of 10, [72.2%, 100.0%].
- Tasks 05, 10, 15 and 20 are no longer held out, since round 1 saw them (`freeze.md`), so no held-out result is reported.

## Model graders

Blind, with `grader-prompt.md`; identifiers in `grading-key.json`. Graders A (Claude Code) and B (GPT-6.1 Sol) both said yes to 39 programs and disagreed on one, which went to grader C (Claude Opus 5.5):

| Program | Grader A | Grader B | Grader C | Mechanical |
| --- | --- | --- | --- | --- |
| Claude Code 11-calendar (qh5m592) | yes | no | no | pass |

The session fixed the leap-year rule as the reference does, and also rewrote `days_between` as a closed form. B and C say the closed form breaks at the smallest `Int`: `leap_years_before(from)` computes `from - 1`. The task asks that every function of `Calendar` be right for every year. A `mo test` probe run on the frozen binary:

| Call | Reference | The program |
| --- | --- | --- |
| `days_between(-9223372036854775808, -9223372036854775807)` | 366 | stops: `bug: overflow: Int` in `leap_years_before` |
| `days_between(-401, 0)` | 146462 | 146462 |

So B and C are right, as decided by running the input in dispute (D275). The program keeps its mechanical pass for this round (protocol section 4), and this is a finding against 11-calendar's hidden tests (finding 2).

Both graders also said yes to the two 02-col-sum programs that fail the hidden tests. Grader A wrote that skipping blank lines before the header is "consistent with" the task, and that the reference does it too; the reference does not. Round 1's graders split on the same point. This round, neither caught it.

## Findings

1. **First retry misses its target again, and the measure is why.** 26 of 37 (70.3%), against round 1's 29 of 37. Every miss in both rounds is an error the session had not yet edited. Meeting 85% needs a change to the measure, the guide or the target, which is Robert's decision.
2. **11-calendar's hidden tests don't reach the ends of `Int`.** A closed-form `days_between` that overflows at the smallest `Int` passes every test. A next round could add a call there; 11-calendar's task text already asks for every year.
3. **02-col-sum's new runs work.** Both families still skip a blank first line, and both programs now fail. The task text could say outright that a blank first line is the header, as round 1 suggested.
4. **The model graders missed both 02-col-sum failures.** Agreement between A and B is not evidence of correctness against a rule the hidden tests check. Only the hidden tests caught these.
