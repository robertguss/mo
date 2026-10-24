# The three tools' final round: results

Six fresh sessions, one per tool per family, on mo 522c4c8 (`freeze.md`), 24 Oct 2026. This is the round D229 measures: at least 5 of the 6 attempts pass. Robert does not grade programs himself (D275), so the hidden tests decide each verdict and the model graders' answers raise findings only.

## Verdicts

| Tool | Claude Code | GPT-6.1 Sol |
| --- | --- | --- |
| linkcheck | fail at step 3: 21 of 23, tests 21 and 22 | pass: 23 of 23 |
| notioncopy | pass: 22 of 22 | pass: 22 of 22 |
| tally | pass: 19 of 19 | pass: 19 of 19 |

All six parse (step 1) and check with no diagnostic (step 2). No session hit E1103, E1150 or E1400, so there is no toolchain gap.

- Both families: 5 of 6 pass, 83.3%, 95% Wilson interval [43.6%, 97.0%]. D229's target is met as a point-estimate pass: even 6 of 6 could not do better than that (protocol section 7).
- Claude Code: 2 of 3, [20.8%, 93.9%]; 1 fails at step 3. GPT-6.1 Sol: 3 of 3, [43.9%, 100.0%].
- Per task: notioncopy and tally passed for both families; linkcheck for GPT-6.1 Sol only. With three tasks, the families' difference is a hint, not a finding.
- Against the diagnostic round (4 of 6): GPT-6.1 Sol's notioncopy, a toolchain gap then (E1150 on `Byte.to_int`), and its linkcheck, too slow then (144.8 s on test 20), both pass. Claude Code's linkcheck fails tests 21 and 22, which were added for this round (D275); it makes the same two mistakes as in the diagnostic round.

Claude Code's linkcheck fails on rules the task states. Test 21: it never checks an empty `#` fragment, so `[top](#)` and `[doc](doc.md#)` are not reported (rule 5). Test 22: it keeps every character above code 127 in anchors, so fullwidth `？` and `：` stay in and three good links are reported broken (rule 6).

## Model graders

Blind, with `grader-prompt.md`; identifiers in `grading-key.json`.

| Program | Grader A (Claude Code) | Grader B (GPT-6.1 Sol) | Grader C (Claude Opus 5.5) | Mechanical |
| --- | --- | --- | --- | --- |
| Claude Code linkcheck (q2fcu7n) | no | no | — | fail |
| Claude Code notioncopy (qnxknxy) | yes | no | yes | pass |
| Claude Code tally (qs3szas) | yes | yes | — | pass |
| GPT-6.1 Sol linkcheck (q8dzhm5) | yes | no | no | pass |
| GPT-6.1 Sol notioncopy (qvjrjcu) | yes | yes | — | pass |
| GPT-6.1 Sol tally (qnyekzg) | yes | yes | — | pass |

Four programs got the same answer from both graders and the tests. The two disagreements went to grader C with both graders' reasons (`grader-c-input.md`). The first grader C session failed before reading anything and was started again (`notes.md`).

- **Settled with the tests:** Claude Code's notioncopy. B is right that a separator line with a tab, `| ---\t| --- |`, is kept as a data row; C counts it a narrow edge, since every ordinary dash line is dropped as rule 7 says.
- **Graders against the tests:** GPT-6.1 Sol's linkcheck. B and C say no: heading text is not trimmed, so `# Hello ` gets the anchor `hello-` and `[x](#hello)` is reported as `no such heading`, against rule 6 (the reference trims, as GitHub does). Run on that input at 522c4c8, the program prints `README.md:5: #hello: no such heading`; the reference prints `ok: 1 links in 1 files`. So the graders are right, as decided by running the input in dispute (`freeze.md`, D275). The program keeps its mechanical pass for this round (protocol section 4), and this is a finding against linkcheck's hidden tests, which have no heading with trailing spaces (finding 1). The diagnostic round's grader C found the same mistake in that round's GPT-6.1 Sol linkcheck.

## First retry

`python3 learnability/retry.py` over the six logs, against the targets without E1103, E1150 and E1400: 10 of 10 errors fixed on the next run, [72.2%, 100.0%], and 10 of 10 within three, a point-estimate pass of both the 85% and 95% targets. No error had those codes, so `tally`'s own count is the same. Six of the ten came from Claude Code's tally session, most of them errors it planted on purpose (finding 2); without it, 4 of 4, [51.0%, 100.0%]. No `mo fix` ran. The sessions' last `mo test` runs passed 108 of 109; the one failure is in Claude Code's tally session's scratch test, not its program, whose last test run on `tally.mo` passed 27 of 27.

## Findings

1. **linkcheck's hidden tests have no heading with trailing spaces.** GPT-6.1 Sol's linkcheck passes 23 of 23 while giving `# Hello ` the anchor `hello-`, in both rounds. A next round should add the case.
2. **A session's own test inputs count as its errors.** Claude Code's tally session wrote Mo files with planted errors under `/tmp/s` to make session logs for its tests, and ran `mo check` on them; the harness logs a run on any file, so `tally` counts their E0053, E0041 and E0078 as the session's errors. Here they were all fixed at once, so no rate changes, but a later round could log only runs on files under the work folder.
3. **The reference has no Unicode letter test.** Rule 6 keeps letters and digits; `Unicode16` offers `lowercase` but nothing that says whether a character is a letter, so a program writes its own table of letter ranges (GPT-6.1 Sol's does) or guesses (Claude Code's keeps everything above code 127). A `Char` letter test, or one in `Unicode16`, would remove the guess.
4. **Stumbles were few.** Ten errors across six sessions, all fixed on the next run: E0093 (an unused import) twice, E0045 (a name clashes with one always in scope), E0068 (a catch-all choice arm), E0074 (a condition that isn't a yes-or-no answer), and the five planted errors of finding 2 (E0053 three times, E0041, E0078). The graders' stumbles lists are in `grader-a.md` and `grader-b.md`.
5. **The diagnostic round's fixes held.** No session met E1150 (MO-113, MO-115), and GPT-6.1 Sol's linkcheck ran test 20 in 4.4 s against its 10-second limit (MO-114; Claude Code's took 5.5 s, timed on this orb outside `grade.py`).
