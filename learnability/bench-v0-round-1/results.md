# Benchmark v0, round 1: results

Forty fresh sessions, one per task per family, on mo 267370f (`freeze.md`), 24 Oct 2026. This round is Phase 2's baseline, which later phases are compared with; it has no pass target of its own (protocol section 7). Robert does not grade programs himself (D275), so the hidden tests decide each verdict and the model graders' answers raise findings only.

## Verdicts

Every one of the forty programs passes `bench/v0/grade.py`: it parses (step 1), checks with no diagnostic (step 2), and passes every hidden test (step 3).

| Task | Kind | Held out | Claude Code | GPT-6.1 Sol |
| --- | --- | --- | --- | --- |
| 01-rle | write |  | pass | pass |
| 02-col-sum | write |  | pass | pass |
| 03-brackets | write |  | pass | pass |
| 04-line-counts | write |  | pass | pass |
| 05-ranges | write | yes | pass | pass |
| 06-temperature | fix |  | pass | pass |
| 07-grades | fix |  | pass | pass |
| 08-playlist | fix |  | pass | pass |
| 09-rpn | fix |  | pass | pass |
| 10-restock | fix | yes | pass | pass |
| 11-calendar | failing-test |  | pass | pass |
| 12-search | failing-test |  | pass | pass |
| 13-titles | failing-test |  | pass | pass |
| 14-wallet | failing-test |  | pass | pass |
| 15-parking | failing-test | yes | pass | pass |
| 16-todo | extend |  | pass | pass |
| 17-stats | extend |  | pass | pass |
| 18-store | extend |  | pass | pass |
| 19-cart | extend |  | pass | pass |
| 20-timesheet | extend | yes | pass | pass |

95% Wilson intervals:

- All forty: 40 of 40, [91.2%, 100.0%].
- Each family: 20 of 20, [83.9%, 100.0%].
- The sixteen tasks that were not held out, both families: 32 of 32, [89.3%, 100.0%].
- **Held-out tasks** (05, 10, 15, 20), reported separately: both families 8 of 8, [67.6%, 100.0%]; each family 4 of 4, [51.0%, 100.0%]. The two models' release dates are not recorded, so whether the held-out tasks postdate them, as protocol section 6 asks, is not shown (`freeze.md`).
- Each kind, both families: 10 of 10, [72.2%, 100.0%].

With every attempt passing, the round cannot tell the families or the task kinds apart. Every interval's top is 100%; its bottom says how much the round could still be hiding.

## Model graders

Blind, with `grader-prompt.md`; identifiers in `grading-key.json`. Graders A (Claude Code) and B (GPT-6.1 Sol) both said yes to 38 programs. They disagreed on two, which went to grader C (Claude Opus 5.5):

| Program | Grader A | Grader B | Grader C | Mechanical |
| --- | --- | --- | --- | --- |
| Claude Code 02-col-sum (qxjfrmq) | yes | no | no | pass |
| GPT-6.1 Sol 02-col-sum (qxdevf4) | yes | no | no | pass |

B and C say no for the same reason in both: the program skips blank lines before taking the header. The task says the first line is the header (rule 1), and `no header` is only for no input at all (rule 6). Run on the frozen binary on the inputs in dispute:

| Input | Reference | Both programs |
| --- | --- | --- |
| `\n1\n` | `: 1.00`, status 0 | `1: 0.00`, status 0 |
| `\n` | `: 0.00`, status 0 | `no header` on standard error, status 1 |

So B and C are right, as decided by running the inputs in dispute (D275). Both programs keep their mechanical pass for this round (protocol section 4), and this is a finding against 02-col-sum's hidden tests (finding 1). Grader A called skipping blank lines before the header a reasonable reading of rule 3; rule 3 sits among the rules about rows, and rule 4 counts skipped lines alongside the header, so the task's wording supports B and C, though not unmistakably.

## First retry

`python3 learnability/retry.py` over the forty logs, against the targets without E1103, E1150 and E1400:

- Both families: 29 of 37 errors fixed on the next run, 78.4%, [62.8%, 88.6%], **missing** the 85% target; 37 of 37 within three, [90.6%, 100.0%], a point-estimate pass of the 95% target.
- Claude Code: 15 of 15 on the next run, [79.6%, 100.0%]; 15 of 15 within three.
- GPT-6.1 Sol: 14 of 22 on the next run, 63.6%, [43.0%, 80.3%]; 22 of 22 within three. All eight errors not fixed on the next run were its: E0051 and E0075 twice each, E0050, E0053, E0068 and E0077 once. The most came from its 07-grades and 10-restock sessions, five errors each; without 07-grades, 28 of 32 for both families, 87.5%, [71.9%, 95.0%].

Counting every code, as `tally` does: 35 of 43, [67.4%, 90.3%], and 43 of 43 within three. No `mo fix` ran. Every session's last `mo test` runs passed, 648 of 648.

## Toolchain gaps

Six errors were on forms the toolchain had not finished, all fixed by the sessions on their next run by writing the behaviour another way:

- **E1150 on BigInt functions (02-col-sum, both families):** `pow`, `to_int` and `abs` on `BigInt` did not lower at 267370f. They do since #186 (MO-121).
- **E1103 on a lambda parameter's tuple projection (20-timesheet, both families):** `list.find(fn(entry) entry.1 ... end)` on a list whose element type is settled only later in the function. This still happens on main 12af809 and is MO-128.

## Findings

1. **02-col-sum's hidden tests have no blank first line.** Both families' programs skip blank lines before the header and pass every test; on `\n1\n` and on blank-only input they differ from the reference. A next round should add both cases, and the task text could say outright that a blank first line is the header.
2. **A checker gap the benchmark found: MO-128.** Both 20-timesheet sessions met the same E1103 independently, on a tuple projection inside a lambda passed to `find`. Filed with a ten-line reproduction.
3. **First retry misses its target, on one family.** Claude Code fixed every error on the next run; GPT-6.1 Sol fixed 14 of 22. Its slow fixes are ordinary checker errors (E0051 a name bound again, E0075 arithmetic on a non-number, E0050 an unknown dot call), not toolchain gaps, so the guide or the diagnostics' wording is where to look.
4. **The ceiling is reached.** Forty passes out of forty leaves the benchmark no room to show improvement. A next round needs harder tasks or tighter hidden tests to measure Phase 3 against this baseline.
