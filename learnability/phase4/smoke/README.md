# Native-client smoke on the evaluation candidate

Development evidence, never scored. On 2026-11-07, one Claude Code session and one Codex session ran through the real harness (`harness.py run`) as the session user `mo-session`. Each was given a task that names every Phase 4 instrument. The purpose was to show that both pinned clients can discover and complete each instrument through the harness before any scored scenario exists. A scored evaluation may not count these sessions, and its scenarios are not this task.

## Setup

| What | Value |
| --- | --- |
| Candidate | `git archive` of commit `176624f6`, built with `make CC=gcc build/gcc/debug/mo` under `/opt/mo-phase4/candidate` and owned by root; `mo 0.1.0`, SHA-256 `f586c0318e4f79fcd57ca34160fe9ba91f6d27182f83231e26349ff4d077d4f2` |
| Claude Code | 2.1.317, SHA-256 `eb224419e344…`, the pilots' pinned install |
| Codex | 0.166.0, SHA-256 `b8a11fedf24b…`, the pilots' pinned install |
| Variant | the public synthetic variant (`testdata/variants/synthetic`) with its report replaced by a six-step task: count jobs by state with the operator tools; fetch a state export; add a function and run it with `mo query`; read the stopped folder with `mo history`; replay the retained failure against the deployed source; fix the defect and check it with `mo replay --fix` |
| Limit | 1,800 seconds per session |

The client's own model and settings apply, since the harness passes no model flag. `results/` holds each session's `manifest.json` and `grade.json`, published with `publish.py` and scanned with `scan.py` against the harness's tokens and stored credentials (`scanned 4 files: clean`). The streams, proxy logs and working folders stay private.

## Results

Both sessions completed every step and were graded successes. Neither was stopped at the limit, and the proxy log and the client stream matched.

| | Claude Code | Codex |
| --- | --- | --- |
| Seconds | 66.2 | 56.9 |
| Operator tools used | `mo-overview`, `mo-state` | `mo-state` |
| Offline commands used | fetch, history, query, replay, replay --fix | fetch, history, query, replay, replay --fix |
| Calls paired / counted as `mo fetch`'s | 2 / 1 | 1 / 1 |
| Replay of the retained failure | `reproduced` | `reproduced` |
| Grader's fix check | `verified`, folder judged correct | `verified`, folder judged correct |

The sessions' own reports agree with the records. Each counted Ready 3, Leased 0, Done 1 and Dead 1 from `mo-state`, got `1` from its `mo query` over the fetched export, and found failure 1 in `stats` with `mo history`. Each replayed it to `reproduced` and checked its fix to `verified`. Each also added a regression test that the queue's tests run.

## What this does not show

The task named every instrument and its order, so the sessions show that the instruments can be discovered and work through both clients. They don't show that an agent would choose an instrument unprompted, or that it would help in a harder incident. The scored protocol's scenarios measure that.
