# Phase 4 pilots: the calibration ledger

Pilots decide whether each of the six scenario variants is solvable with today's tools but not
trivial, before anything is frozen. They measure nothing: no pilot counts toward the baseline or
the final round. This ledger keeps every pilot session in the order it ran, kept or not, with
its verdict and anything it changed. What a change did to a variant is written in the private
repository's notes, never here; this ledger names the change and the note.

From pilot 16 onward, Robert authorized the agent to run the remaining pilots and record
recommended verdicts without individual approval. These are labeled recommendations, not
Robert’s personal judgments.

## The rule

- Pilots run with the baseline `mo` and nothing from Phase 4, one session at a time, and stop
  at 30 sessions, or sooner once every variant is calibrated.
- The first two pilots are `shared-briefs`, one on each client, as the end-to-end check of the
  harness with real clients. After them, one pilot per variant per client, alternating
  clients.
- A wrong-state or failure variant is calibrated once a pilot has solved it at least once and
  failed it at least once, with at least one session on each client.
- A question variant is calibrated once at least one session on each client has run and its
  answer key has been checked against a built folder (`grade.py --check MO ROOT BUILT`). No
  pilot has to solve it: a baseline near zero there is the predicted result.
- After a variant's third pilot: solved in all three, it is hardened and piloted again; a
  wrong-state or failure variant solved in none is replaced within its family.
- A change to a variant is followed by `grade.py --check` and `--freeze` before its next pilot,
  and restarts that variant's count. The table below keeps the earlier sessions.
- Pilots may change a variant's defect, data, bug report, hidden tests and answer key, the time
  limit and the client settings. They may not change the three families, the number of
  variants, the session counts, the clients or the comparison.
- A variant still uncalibrated at the cap is frozen as it stands and named as such in the
  freeze.

## What the first two pilots check

The harness was tested only with a stub client, so the two `shared-briefs` pilots also check,
from each session's records:

- Codex honours `-c web_search="disabled"`: its stream holds no web search.
- The host check reads Codex's commands through their `bash -lc` wrapping: a command that
  addresses the service shows `127.0.0.1` and nothing else.
- The record match pairs every real tool call in each client's stream with the proxy log.
- The manifest's first assistant event is found for each client, and the grader reads each
  client's last message for its `ANSWER:` line.

Both checked out on 4 Nov 2026: in pilots 1 and 2 the answer was read and graded, every tool
call paired (2 of 2, 124 of 124), the first assistant event was found, and Codex's stream held
no web search. Codex's five commands all came wrapped in `bash -lc`, and the host check read
them without a false alarm; none addressed a host, so catching one inside the wrapping is still
unseen.

The host check reads the commands as written, so a script written to a file and then run is not
checked. Commands are reviewed for that; from pilot 16 onward the agent performed this review under Robert’s authorization.

## Setup

- `mo`: built from `main` at `87948db24aa72c62fb5a05d72fbc1b1c6c2ad211`, which the annotated
  tag `phase4-baseline` names (pushed 4 Nov 2026). It has no source change since Phase 3.
  Exported with `git archive` to `/opt/mo-phase4/baseline` and built there with
  `make CC=gcc build/gcc/debug/mo` (gcc 13.3.0, Ubuntu 24.04) on 4 Nov 2026, so that its
  library path, `/opt/mo-phase4/baseline/lib`, can be read by the session user (see
  `../README.md`). The tree is root's.
  - SHA-256 `af407b466acfe13cfe7438fff625aecd5507bba8998ef866cfc4b33a54342640`, installed as
    `/opt/mo-phase4/bin/mo`.
  - Embedded guide (`docs/agent-guide.md`, and `mo guide`'s output) SHA-256
    `da951023406d083d5e717a7f9f7ba94108882c51b7a162f37e5ad2aa47d2ce7f`.
  - Embedded reference (`docs/reference/README.md`, and `mo guide --reference`'s output)
    SHA-256 `05ea0186022c6728781b7dc70858b13852d667eaf32a7aafb69e3d60c68dc1f9`.
- Clients, installed under `/opt/mo-phase4/bin` by `harness.py setup`:
  - Claude Code 2.1.317, SHA-256
    `eb224419e344575faa4a1f39b290b649425a10cd5ee2ef1d003480cd9ae57eb0`.
  - Codex CLI 0.166.0 (standalone, x86_64 musl), SHA-256
    `b8a11fedf24b2dfb59a70c9e52b030f9a508dcd49cd2dd0d41956f4c785156b8`.
- Logins: Robert logged both clients in as `mo-session` on 4 Nov 2026 (Claude Code with the
  claude.ai subscription, Codex with ChatGPT through `--device-auth`). The login shell had no
  `DISABLE_AUTOUPDATER=1`, so Claude Code installed 2.1.318 into the session user's home; that
  copy was deleted and the snapshot taken again, so every session starts with the pinned
  2.1.317 alone. The login steps now set the variable.
- Variants: `robertguss/mo-phase4-private` at `1331ded`.
- Data folders: built with `harness.py build` and the baseline `mo` into
  `~/phase4-builds/pilots-1` on 4 Nov 2026; all six built, `expire-one-per-tick` in 32 minutes
  for its lapse, the others in 1 to 3. `grade.py --check /opt/mo-phase4/bin/mo
  ~/mo-phase4-private ~/phase4-builds/pilots-1` then passed for all six: each reference fix
  passes, each defect variant and its broken fix fail a hidden test, both answer keys reproduce
  from the built folders, and every `SHA256SUMS` is current.
- Time limit: 3600 seconds.

## Sessions

Each session's `manifest.json` and `grade.json` are in `NN-VARIANT-CLIENT/`, copied by
`../publish.py` and passed by `../scan.py`. Its stream, proxy log, service log and folders stay
in Robert's private tree. Grade is the harness's: the grader, the record match and the host
check together. Verdict is Robert's reading of the session. Minutes are the manifest's
`seconds`. Records are the record match.

| # | Date | Variant | Client | Grade | Verdict | Minutes | Records | Changed |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 4 Nov 2026 | `shared-briefs` | Claude Code | success | Solved, by reading the start state instead of the service: it decoded the saved snapshot from the read-only data copy and confirmed with one `list` call. A sign that `shared-briefs` may be trivial as designed; judged after its third pilot. | 1.1 | 2 of 2 matched | nothing |
| 2 | 4 Nov 2026 | `shared-briefs` | Codex | success | Solved, the intended way: paged through every job with `list` (122 calls) and counted. It took about a minute, so paging cost is no barrier. With pilot 1, a sign that `shared-briefs` is too easy for the baseline. | 1.1 | 124 of 124 matched | nothing |
| 3 | 4 Nov 2026 | `expire-one-per-tick` | Claude Code | success (regraded) | Solved: a correct one-line fix in under 5 minutes, found by reading the source, with no call to the service. Added an example, a promise and a regression test. Initially failed because the grader refused added checks; passed after the grader allowed them. The harness crashed before writing the manifest, which was recovered from the records with unknown fields marked null. Robert approved this verdict. | 4.9 | 0 of 0 matched | grader accepts added checks; host check handles refused-command events |
| 4 | 4 Nov 2026 | `expire-kills-third-attempt` | Codex | success | Solved: a correct one-line fix in 2 minutes, found by reading the source, with no call to the service. Added regression tests to `queue-tests.mo`, which the grade ignores. Robert approved this verdict. | 1.9 | 0 of 0 matched | nothing |
| 5 | 4 Nov 2026 | `who-killed-the-job` | Claude Code | success | Solved in about a minute by decoding the saved call log and snapshot, with one service call to confirm the current state; no code changes. Robert approved this verdict. | 1.0 | 1 of 1 matched | nothing |
| 6 | 4 Nov 2026 | `list-pasted-brief` | Codex | success | Solved in about 2 minutes after reading source and logs and making two service calls. Added a regression test that reproduced the failure before the fix and passed afterwards; all 48 checks passed. Robert approved this verdict. | 2.0 | 2 of 2 matched | nothing |
| 7 | 4 Nov 2026 | `stats-quadratic` | Claude Code | incomplete: operator interrupted | Saw the task and was testing a fix when the operator pressed Ctrl-C after a proxy connection-reset traceback. No final result, manifest or grade. Private records and working files preserved; session reset. Robert approved: operator-interrupted; follow-up completed successfully as pilot 9. This is not evidence of scenario difficulty. | unknown | not assessed | nothing |
| 8 | 4 Nov 2026 | `stats-quadratic` | Codex | success | Solved in 6 minutes after reading source and making one service call. Added a maximum-size regression test, confirmed it caught the original bug, and passed all 48 checks with the fix. Robert approved this verdict. | 6.0 | 1 of 1 matched | nothing |
| 9 | 4 Nov 2026 | `stats-quadratic` | Claude Code | success | Solved in about 14 minutes after reading source and making one service call. Added a maximum-size regression test; all 48 checks passed with the fix, and the regression test caught the original bug in a scratch copy. Most session time was spent running tests. Robert approved this verdict. | 14.0 | 1 of 1 matched | nothing |
| 10 | 5 Nov 2026 | `expire-one-per-tick` | Codex | success | Solved in about 2 minutes after reading source and making one service call. Two new regression tests failed before the fix; all 49 checks passed afterwards, including recovery coverage. Robert approved this verdict. | 2.1 | 1 of 1 matched | nothing |
| 11 | 5 Nov 2026 | `expire-kills-third-attempt` | Claude Code | success | Solved in under 3 minutes by reading source and logs, with no service call. Added two regression examples and a simulation test; all 50 queue checks passed. Robert approved this verdict. | 2.7 | 0 of 0 matched | nothing |
| 12 | 5 Nov 2026 | `who-killed-the-job` | Codex | success | Solved in 29 seconds by decoding the saved history, with no service call. Robert approved this verdict. | 0.5 | 0 of 0 matched | nothing |
| 13 | 5 Nov 2026 | `list-pasted-brief` | Claude Code | success | Solved in about 2 minutes after reading source and making two service calls. Added a regression test; all 48 queue checks passed. Robert approved this verdict. | 2.0 | 2 of 2 matched | nothing |
| 14 | 5 Nov 2026 | `expire-one-per-tick` | Codex | success | Solved in about 2 minutes by reading source and logs, without service calls. Two regressions reproduced the bug before the fix; all 49 checks passed afterwards. Robert approved this verdict; third solve triggers hardening. | 2.1 | 0 of 0 matched | nothing |
| 15 | 5 Nov 2026 | `expire-one-per-tick` | Claude Code | success | Solved the revised report in about 5½ minutes using source, simulation and five service calls. Added regression coverage; all 49 queue checks passed. Robert approved this verdict. The report revision did not prevent a solve. | 5.6 | 5 of 5 matched | nothing |
| 16 | 5 Nov 2026 | `expire-one-per-tick` | Codex | success | Recommended: solved; grader passed. 5 recorded service calls. Commands reviewed; no foreign-host activity found. | 2.1 | 5 of 5 matched | nothing |
| 17 | 5 Nov 2026 | `expire-kills-third-attempt` | Claude Code | success | Recommended: solved; grader passed. 0 recorded service calls. Commands reviewed; no foreign-host activity found. | 1.8 | 0 of 0 matched | nothing |
| 18 | 5 Nov 2026 | `list-pasted-brief` | Codex | success | Recommended: solved; grader passed. 2 recorded service calls. Commands reviewed; no foreign-host activity found. | 2.0 | 2 of 2 matched | nothing |
| 19 | 5 Nov 2026 | `stats-quadratic` | Claude Code | success | Recommended: solved; grader passed. 1 recorded service calls. Commands reviewed; no foreign-host activity found. | 8.3 | 1 of 1 matched | nothing |
| 20 | 5 Nov 2026 | `expire-kills-third-attempt` | Codex | success | Recommended: solved; grader passed. 3 recorded service calls. Commands reviewed; no foreign-host activity found. | 3.0 | 3 of 3 matched | nothing |
| 21 | 5 Nov 2026 | `list-pasted-brief` | Claude Code | success | Recommended: solved; grader passed. 4 recorded service calls. Commands reviewed; no foreign-host activity found. | 2.2 | 4 of 4 matched | nothing |
| 22 | 5 Nov 2026 | `stats-quadratic` | Codex | success | Recommended: solved; grader passed. 7 recorded service calls. Commands reviewed; no foreign-host activity found. | 2.1 | 7 of 7 matched | nothing |
| 23 | 5 Nov 2026 | `expire-kills-third-attempt` | Claude Code | success | Recommended: solved; grader passed. 2 recorded service calls. Commands reviewed; no foreign-host activity found. | 3.2 | 2 of 2 matched | nothing |
| 24 | 5 Nov 2026 | `list-pasted-brief` | Codex | success | Recommended: solved; grader passed. 9 recorded service calls. Commands reviewed; no foreign-host activity found. | 2.5 | 9 of 9 matched | nothing |
| 25 | 5 Nov 2026 | `stats-quadratic` | Claude Code | success | Recommended: solved; grader passed. 3 recorded service calls. Commands reviewed; no foreign-host activity found. | 2.1 | 3 of 3 matched | nothing |
| 26 | 5 Nov 2026 | `expire-one-per-tick` | Codex | success | Recommended: solved; grader passed. 3 recorded service calls. Commands reviewed; no foreign-host activity found. | 2.3 | 3 of 3 matched | nothing |
| 27 | 5 Nov 2026 | `expire-kills-third-attempt` | Claude Code | success | Recommended: solved; grader passed. 2 recorded service calls. Commands reviewed; no foreign-host activity found. | 1.7 | 2 of 2 matched | nothing |
| 28 | 5 Nov 2026 | `list-pasted-brief` | Codex | success | Recommended: solved; grader passed. 5 recorded service calls. Commands reviewed; no foreign-host activity found. | 3.0 | 5 of 5 matched | nothing |
| 29 | 5 Nov 2026 | `stats-quadratic` | Claude Code | success | Recommended: solved; grader passed. 1 recorded service calls. Commands reviewed; no foreign-host activity found. | 5.0 | 1 of 1 matched | nothing |
| 30 | 5 Nov 2026 | `expire-one-per-tick` | Codex | success | Recommended: solved; grader passed. 2 recorded service calls. Commands reviewed; no foreign-host activity found. | 2.4 | 2 of 2 matched | nothing |

## Changes

| After pilot | Variant | What changed | Private note |
| --- | --- | --- | --- |
| 3 | instrumentation | Grader permits added checks while retaining the variant’s original grading tests; host check handles refused-command events. No variant change. | none: instrumentation only |
| 14 | `expire-one-per-tick` | Revised the operator report as a hardening experiment; source, fixture and grading criteria unchanged. Refreshed hashes and passed `grade.py --check`: 6 of 6 variants sound, including question keys. Calibration count restarted; earlier sessions retained. | `notes/pilot-14-expire-one-per-tick.md` in the private repository |
| 17 | `expire-kills-third-attempt` | Revised operator report after three completed solves. Source, fixture and grading criteria unchanged. Refreshed hashes; full validation passed, including question keys. Calibration count restarted. | `notes/pilot-17-expire-kills-third-attempt-revision-2.md` in the private repository |
| 18 | `list-pasted-brief` | Revised operator report after three completed solves. Source, fixture and grading criteria unchanged. Refreshed hashes; full validation passed, including question keys. Calibration count restarted. | `notes/pilot-18-list-pasted-brief-revision-2.md` in the private repository |
| 19 | `stats-quadratic` | Revised operator report after three completed solves. Source, fixture and grading criteria unchanged. Refreshed hashes; full validation passed, including question keys. Calibration count restarted. | `notes/pilot-19-stats-quadratic-revision-2.md` in the private repository |
| 26 | `expire-one-per-tick` | Further report revision after three solves on the previous report. Source, fixture and grading criteria unchanged. Refreshed hashes and passed full validation. Calibration count restarted. | `notes/pilot-26-expire-one-per-tick-revision-3.md` in the private repository |
| 27 | `expire-kills-third-attempt` | Further report revision after three solves on the previous report. Source, fixture and grading criteria unchanged. Refreshed hashes and passed full validation. Calibration count restarted. | `notes/pilot-27-expire-kills-third-attempt-revision-3.md` in the private repository |
| 28 | `list-pasted-brief` | Further report revision after three solves on the previous report. Source, fixture and grading criteria unchanged. Refreshed hashes and passed full validation. Calibration count restarted. | `notes/pilot-28-list-pasted-brief-revision-3.md` in the private repository |
| 29 | `stats-quadratic` | Further report revision after three solves on the previous report. Source, fixture and grading criteria unchanged. Refreshed hashes and passed full validation. Calibration count restarted. | `notes/pilot-29-stats-quadratic-revision-3.md` in the private repository |

## Summary

Solves and failures are counted since the variant's last change.

| Variant | Family | Claude sessions | Codex sessions | Solved | Failed | Verdict |
| --- | --- | --- | --- | --- | --- | --- |
| `expire-one-per-tick` | Wrong state | 0 | 1 | 1 | 0 | uncalibrated at cap: final report solved in pilot 30; no failure |
| `expire-kills-third-attempt` | Wrong state | 0 | 0 | 0 | 0 | uncalibrated at cap: final report revision validated but not piloted |
| `shared-briefs` | Question, state | 1 | 1 | 2 | 0 | calibrated by the rule, but too easy: hardening to be decided before the freeze |
| `who-killed-the-job` | Question, history | 1 | 1 | 2 | 0 | calibrated by the question rule; both clients solved |
| `list-pasted-brief` | Failure, defect | 0 | 0 | 0 | 0 | uncalibrated at cap: final report revision validated but not piloted |
| `stats-quadratic` | Failure, work limit | 0 | 0 | 0 | 0 | uncalibrated at cap: final report revision validated but not piloted |

Pilot sessions recorded: 30 of 30 (29 completed, 1 operator-interrupted). Pilot 7 is recorded separately as operator-interrupted, followed up by pilot 9; it is a stopped-session failure under protocol section 6, but is not used as evidence of scenario difficulty in the calibration counts above.

## Final outcome at the cap

Pilots stopped at 30 sessions. All 29 completed sessions received a successful harness grade;
pilot 7 was operator-interrupted and its records were preserved. Recommended verdicts for
pilots 16–30 are solves, with no individual Robert approval implied. Every completed session’s
record match passed. Pilot 3’s recovered manifest retains unknown fields as null, as documented
in its session row; it was not a normal harness completion.

The two question variants meet their calibration rule: both clients ran, and their answer keys
were recomputed from the built folders in the final soundness check. The four wrong-state and
failure variants remain **uncalibrated at the cap**. None of their completed pilots failed.

Hardening was limited to operator-report revisions. The source, fixtures and grading criteria
were unchanged. Tested revisions continued to be solved; this does not demonstrate adequate
scenario difficulty. The last revisions after pilots 26–29 passed validation, but only the
`expire-one-per-tick` revision received a further pilot (30) before the cap. The other three
final report revisions have no pilot evidence. The summary counts are since each last revision;
the session table retains every earlier run.

Final verification: `grade.py --check /opt/mo-phase4/bin/mo ~/mo-phase4-private
~/phase4-builds/pilots-1` exited 0 with `phase 4 variants: 6 of 6 sound`, including both question
keys. Each changed report’s `SHA256SUMS` was refreshed with `--freeze`. No baseline round was run in this work. These four uncalibrated classifications remain part of the evidence. The 2026-11-05 amendment in the canonical Phase 4 plan supersedes the old baseline prerequisite; see [findings](findings.md).
