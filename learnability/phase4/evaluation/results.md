# Phase 4 diagnostic evaluation results

**Gate: pass.** 14 of 16 sessions correct; Robert approved v0.1.0 on 2026-11-07.

All 16 slots ran once on 2026-11-07, under the [frozen protocol](../diagnostic-protocol.md).
No baseline was collected. This is evidence of capability on these four scenarios, not of
improvement over an earlier toolchain or a population success rate.

## Gate

| Condition | Observed | Result |
| --- | --- | --- |
| At least 12 of 16 correct | 14 of 16 | Pass |
| At least 2 of 4 in every scenario | A 4, B 4, C 4, D 2 | Pass |
| At least 5 of 8 on each client | Codex 8, Claude Code 6 | Pass |
| No original-first violation | 0 | Pass |
| Publication scan clean | Manifests/grades and evidence stage/extraction clean | Pass |

## Sessions

| Slot | Scenario | Client | Seconds | Correct | Operator tools | Offline commands |
| --- | --- | --- | ---: | --- | --- | --- |
| [01](01-completions-from-staging-codex/grade.json) | C | codex | 177.4 | yes | mo-failures, mo-overview, mo-supervisor | mo history, mo query, mo replay, mo replay --fix |
| [02](02-completions-from-staging-claude/grade.json) | C | claude | 170.1 | yes | mo-failures | none |
| [03](03-last-acknowledge-codex/grade.json) | A | codex | 42.8 | yes | mo-log, mo-overview, mo-state | mo history |
| [04](04-lease-past-its-time-codex/grade.json) | D | codex | 200.9 | yes | mo-failures, mo-overview, mo-supervisor | mo history, mo replay, mo replay --fix |
| [05](05-completions-from-staging-claude/grade.json) | C | claude | 172.1 | yes | mo-failures | mo history |
| [06](06-waiting-bytes-claude/grade.json) | B | claude | 63.3 | yes | none | mo fetch, mo query |
| [07](07-last-acknowledge-codex/grade.json) | A | codex | 46.4 | yes | mo-log, mo-overview | mo history |
| [08](08-last-acknowledge-claude/grade.json) | A | claude | 50.9 | yes | mo-log, mo-overview | mo history, mo query, mo replay |
| [09](09-last-acknowledge-claude/grade.json) | A | claude | 61.3 | yes | mo-log, mo-overview | mo history |
| [10](10-waiting-bytes-claude/grade.json) | B | claude | 66.7 | yes | none | mo fetch, mo query |
| [11](11-waiting-bytes-codex/grade.json) | B | codex | 117.5 | yes | mo-overview, mo-state | mo fetch, mo history, mo query |
| [12](12-waiting-bytes-codex/grade.json) | B | codex | 164.0 | yes | mo-overview | mo fetch |
| [13](13-lease-past-its-time-codex/grade.json) | D | codex | 209.9 | yes | mo-failures, mo-overview | mo history, mo replay, mo replay --fix |
| [14](14-completions-from-staging-codex/grade.json) | C | codex | 191.6 | yes | mo-failures, mo-overview, mo-supervisor | mo history, mo query |
| [15](15-lease-past-its-time-claude/grade.json) | D | claude | 986.0 | no | mo-failures, mo-timers | none |
| [16](16-lease-past-its-time-claude/grade.json) | D | claude | 1045.6 | no | none | mo history |

## Demonstration

Counts below include only correct sessions with the instrument in their recorded coverage.
An instrument is demonstrated on a client when that count is at least one. Coverage can miss
commands run inside scripts written by the session; absence is not proof of non-use.

| Instrument | Codex | Claude Code |
| --- | ---: | ---: |
| operator tools | 8 | 4 |
| mo fetch | 2 | 2 |
| mo history | 7 | 3 |
| mo query | 3 | 3 |
| mo replay | 3 | 1 |
| mo replay --fix | 3 | 0 |

`mo replay --fix` was demonstrated on Codex, but not on Claude Code in a correct scored
session. The grader’s separate fix check does not count as a session using the command.

## Fix checks and misses

All eight defect sessions’ originals reproduced, and all eight revised folders received
`verified` from the grader’s fix check. Six passed the hidden tests; slots 15 and 16 did not.
There were zero original-first violations.

Both misses were Claude Code attempts on scenario D. Each exited normally, before the limit,
and failed a hidden test after the queue’s original tests were restored. Each received a
`verified` fix-check verdict because the reconstructed failure was gone. That verdict did not
establish application correctness, and neither miss was replaced.

The proxy logged HTTP connection-reset exceptions during slots 15 and 16. Both sessions’
client/proxy tool records matched. All 16 record matches passed; no foreign-host command,
missing session, timeout, discarded fixture, unstarted attempt or model substitution was recorded.

## Predictions against results

The predictions in protocol section 9 remain unchanged.

| Prediction | Observed | Assessment |
| --- | --- | --- |
| A: 3 of 4 correct | 4 of 4 | Count exceeded prediction |
| A: history/log in at least 3 | 4 | Held |
| B: 4 of 4 correct | 4 of 4 | Held |
| B: fetch and query in at most 2 | Both recorded in 3; fetch in all 4 | Missed |
| B: the rest page through author tools | All 4 recorded fetch | Missed |
| C: 4 of 4 correct | 4 of 4 | Held |
| C: history in at least 2 | 3 | Held |
| C: replay in at most 2 | 1 | Held |
| D: 3 of 4 correct | 2 of 4 | Missed |
| D: replay --fix in at most 2 | 2 | Held |
| Overall: 14 of 16 correct | 14 of 16 | Held |
| At least 7 of 8 on each client | Codex 8, Claude Code 6 | Missed |
| No original-first violation; gate passes | 0; pass | Held |

## Publication

[Evidence archive and private committed history](https://github.com/robertguss/mo/releases/tag/evidence-phase4-diagnostic-2026-11-07). The history is a plain git fast-export
stream through private commit `fda0ce0`; restoration preserves its commit identity and every
scenario hash in `freeze.json`. Uncommitted private edits are excluded. The archive includes
every session stream, proxy log, original grade/manifest, stderr/service log, and the round log.

- Initial manifest/grade scan: `scanned 33 files: clean`.
- Final evidence stage scan: `scanned 107 files: clean`.
- Extracted archive scan: `scanned 107 files: clean`.

Archive SHA-256: `bdd71fc10070c52cec1c5b2c2418bf36c0b99894ac7c5f64fd769e1168d04739`.

The publication scan named by protocol section 8 item 5 is the scan specified in section 11;
its reference to section 10 is a cross-reference typo. No threshold or procedure was changed.
