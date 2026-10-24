# The three tools' diagnostic round: results

Six fresh sessions, one per tool per family, on mo f27cac3 (`freeze.md`), 24 Oct 2026. This round is diagnostic: it finds what to fix in the guide, the messages and the toolchain before the final round, and it is not measured against D229's target. Robert chose not to grade this round's programs himself (protocol section 5), so the model graders' answers raise findings only.

## Verdicts

| Tool | Claude Code | GPT-6.1 Sol |
| --- | --- | --- |
| linkcheck | pass: 21 of 21 tests | fail at step 3: 20 of 21, test 20 took 144.8 s against its 10 s limit |
| notioncopy | pass: 22 of 22 | toolchain gap: E1150 on `Byte.to_int`, its only diagnostic; no test could run |
| tally | pass: 19 of 19 | pass: 19 of 19 |

All six parse (step 1). Five check with no diagnostic (step 2).

- Both families: 4 of 6 pass, 66.7%, 95% Wilson interval [30.0%, 90.3%]; 1 fails at step 3; 1 is a toolchain gap.
- Claude Code: 3 of 3, [43.9%, 100.0%]. GPT-6.1 Sol: 1 of 3, [6.1%, 79.2%], with one fail and one gap.
- Per task: tally passed for both families; linkcheck and notioncopy passed for Claude Code only. With three tasks, the families' difference is a hint, not a finding.

## Model graders

Blind, with `grader-prompt.md`; identifiers in `grading-key.json`.

| Program | Grader A (Claude Code) | Grader B (GPT-6.1 Sol) | Grader C (Claude Opus 5.5) | Mechanical |
| --- | --- | --- | --- | --- |
| Claude Code linkcheck (p353hzt) | yes | no | no | pass |
| Claude Code notioncopy (puet625) | yes | no | yes | pass |
| Claude Code tally (pfmjtu8) | yes | no | yes | pass |
| GPT-6.1 Sol linkcheck (pjemxbj) | yes | no | no | fail |
| GPT-6.1 Sol tally (psxrhx9) | yes | yes | — | pass |

GPT-6.1 Sol's notioncopy never reached step 3, so no grader read it.

- **Settled with the tests:** Claude Code's notioncopy and tally (C: B's points are inputs the task doesn't describe), and GPT-6.1 Sol's linkcheck (C: `# Hello ` gets the anchor `hello-`, breaking rule 6).
- **Robert agreed with the graders:** Claude Code's linkcheck is a no. B and C say no against a full test pass: it never checks an empty `#` fragment (`[x](#)` or `doc.md#`), against rule 5, and it keeps combining marks and fullwidth `？` in anchors, so `# Café ？` (with a combining accent) has no anchor `cafe-` and `# 日本語？` has no anchor `日本語`, against rule 6. Both were confirmed by running it on those inputs at f27cac3; GPT-6.1 Sol's linkcheck handles all of them. It keeps its mechanical pass for this round (protocol section 4), and this is a finding against linkcheck's hidden tests, which have no case for either (finding 6).

## First retry

`python3 learnability/retry.py` over the six logs, against the targets without E1103, E1150 and E1400: 7 of 7 errors fixed on the next run, [64.6%, 100.0%], a point-estimate pass of both the 85% and 95% targets. Five of the seven came from Claude Code's tally session; without it, 2 of 2. Counting every code as `tally` does: 9 of 10, the one unfixed being GPT-6.1 Sol's E1150. No `mo fix` ran. The tests the sessions wrote passed 92 of 92 on their last `mo test`.

## Findings

1. **`Byte.to_int` and `List.sum` still give E1150.** Both notioncopy sessions hit them; Claude Code wrote around both and GPT-6.1 Sol kept `Byte.to_int` and could not run. MO-113 lowers them.
2. **The guide's E1150 rule can cost a whole program.** "If the reference requires the form, leave the form in" made GPT-6.1 Sol keep a call it could have replaced by a few lines. The guide could say to keep the form only when the reference offers no other way to write it.
3. **Appending an `if` expression copies the list.** `xs = xs.append(if c: a else: b)` loses in-place reuse and copies on every call (`findings/append-if-copies.mo`: 13 s at 40,000 appends, 0.06 s with the `if` bound to a name first). It accounts for about 135 of the 145 seconds GPT-6.1 Sol's linkcheck took on test 20.
4. **Stumbles were few.** Seven errors across six sessions: E0052 (an unused name) twice, E0071 (a case used without its type in scope) three times, E0068 (a catch-all choice arm) and E0093 (an unused import) once each, all fixed on the next run. The sessions' own reports and `check-log` summaries are in `grader-a.md` and `grader-b.md`.
5. **Packaging.** The harness log sat outside the orb's workspace, so it was collected by asking each finished session to archive it (`notes.md`). Next round's `setup.sh` should keep it inside the workspace.
6. **linkcheck's hidden tests miss two rules.** No test has an empty `#` fragment (rule 5) or a heading with a combining mark or fullwidth punctuation (rule 6), so a program that gets both wrong passes 21 of 21.

## Still to do

- Finding 1 is fixed by MO-113 (#176). Finding 2 is MO-115 and finding 3 is MO-114.
- Next freeze: add two linkcheck hidden tests, an empty `#` fragment (`[x](#)`, `doc.md#`) and a heading with a combining mark and fullwidth `？` (finding 6).
- Robert's human check (protocol section 5) was skipped for this diagnostic round at his request; it starts in a later round.
