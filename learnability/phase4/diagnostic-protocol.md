# The Phase 4 diagnostic evaluation protocol

**Approved by Robert on 2026-11-07. Not yet frozen.** Collection starts only after `freeze.json` is recorded and the budget decision in section 12 is recorded. Any change after the first scored session starts a new, separately named experiment.

This protocol replaces the evaluation described in [protocol.md](protocol.md), which stays as the historical record of the original experiment and its predictions. It also supersedes an earlier draft of this file: written on 2026-11-05 on the unmerged branch `t3/draft-u6-diagnostic-protocol` (commit `a1fa3a96`), approved by Robert that day, and set aside by him on 2026-11-07. That draft proposed 24 sessions, independent human or model graders and a coverage gate. Robert chose 16 sessions, mechanical grading, and coverage reported but not gated. The plan for this work is [docs/plans/2026-11-07-1234-feat-phase4-evaluation-release-plan.md](../../docs/plans/2026-11-07-1234-feat-phase4-evaluation-release-plan.md).

## 1. The question and the claim

Can fresh agent sessions, given a running Mo service and the Phase 4 instruments, diagnose and fix realistic incidents, or answer questions about the service, correctly? The instruments are the operator views, state export with `mo fetch`, offline `mo query`, stopped-folder `mo history`, and failure replay with original-first fix checking.

The claim is capability only: what the sessions did, with which instruments, and every miss. No baseline is collected, and nothing here supports a claim that the instruments improve on the tools Mo had before. A comparative claim would need its own prospective protocol with an independent baseline (phase plan R22).

## 2. What never counts as a scored session

None of these enters the evaluation's results, whatever its outcome:

- the preflight probes (`preflight/`);
- the 30 pilot attempts (`pilot/`) and their six scenarios, which are retired from scoring;
- the development evidence for the instruments: `live-views/`, `private-fetch/`, `failure-capture/` and `offline/`;
- the native-client smoke on the candidate (`smoke/`, 2026-11-07);
- stub-client harness tests and any session run while building or repairing this protocol.

A scored session is one of the 16 slots in section 4, run after the freeze.

## 3. Scenarios

Four scenarios, one for each instrument the suite must exercise. Each is a task-queue incident with the served queue's source in the session's folder. No source is hidden and nothing is obfuscated. Their contents (sources, fixtures, reports, hidden tests, reference and broken fixes, key rules) are in the private repository `robertguss/mo-phase4-private`, commit `fda0ce0`, under `variants/`, and stay private until the evaluation is published. Robert approved all four on 2026-11-07, after a read-only cross-model review of their keys and hidden tests (Codex `gpt-6.1-sol`, extra-high effort).

| Scenario | Kind | Shape | Graded by |
| --- | --- | --- | --- |
| A | question | about the service's committed history, which no author tool shows | its answer key, from the built folder's log, and the queue's own tests |
| B | question | an aggregate over the whole current state of a large queue | its answer key, from the built service, and the queue's own tests |
| C | defect | a handler defect whose failures are retained in the stopped data folder | the hidden tests and the queue's own tests |
| D | defect | a defect that fires only under a timing condition, where the fix should be checked against the recorded failure | the hidden tests and the queue's own tests |

Every scenario passes `grade.py --check` against the frozen candidate (section 9). For a defect, the reference fix passes the hidden tests, and the unfixed variant and a plausible broken fix each fail one. For a question, the key recomputed from the built folder matches.

## 4. Sessions

**Allocation.** 16 sessions: each of the 4 scenarios twice on each of 2 clients. They run one at a time in the order `harness.py order 20261107 A B C D` gives, using the scenarios' private names; `freeze.json` records the resulting list.

**What a session gets** is what `harness.py` gives it, unchanged from the frozen scripts: its own unix user; a working folder holding the scenario's three source files; the running service through a recording proxy, as the MCP server `task-queue`, with its URL and the token's environment variable; the service's standard error log; a read-only copy of the data folder as the session starts (a stopped service's folder); a read-only copy of the deployed source; and `mo`. The prompt is `prompt.md` with the scenario's report. It does not teach the instruments: `mo guide` does. The web is off, and any command that addresses a host other than the loopback address fails the session.

**Time limit.** 3,600 seconds per session from the client's launch. At the limit the session's processes are ended and its folder graded as it stands.

## 5. Clients and models

| Client | Version and SHA-256 | Model | Settings |
| --- | --- | --- | --- |
| Claude Code | 2.1.317, `eb224419e344…` | `claude-opus-5-5`, given as `--model` | `-p`, `--strict-mcp-config` with the proxy as `task-queue`, `stream-json` output, no session persistence, `acceptEdits`, `Bash` and the server's tools allowed, web tools disallowed, no self-update |
| Codex | 0.166.0, `b8a11fedf24b…` | `gpt-6.1-sol` at reasoning effort `xhigh`, given as `-m` and `-c model_reasoning_effort` | `exec --json --ephemeral`, `workspace-write` sandbox with network access for the loopback service, web search off, approval policy `never`, the server's tools approved, no update check |

These are the client binaries the native-client smoke ran (`smoke/README.md`), pinned under `/opt/mo-phase4`. If either changes before the freeze, the smoke runs again on the new one first. A model name is what the provider serves under that name on the session's date; nothing here shows that the same name serves the same weights on another day, and the claim is limited to the recorded names and dates.

The smoke ran before the harness named the models on the command line. Claude Code's stream shows that its session used `claude-opus-5-5`. Codex's stream does not report a model, so the smoke's Codex model is unrecorded.

## 6. Grading

`harness.py` grades each session as it ends, into `grade.json`.

- **Correct.**
  - For a defect: the scenario's hidden tests and the queue's own tests pass on the session's folder, with the queue's tests restored and its own test, examples and promise blocks unchanged.
  - For a question: the single `ANSWER:` value in the last message equals the key (a number, or a time compared to the second in UTC), and the queue's own tests still pass.
  - Correctness also needs the proxy log and the client stream to hold the same tool calls, and no command to address another host.
- **Coverage**, recorded and never part of correctness: the operator tools the session called through its MCP client, and the offline commands (`mo fetch`, `mo history`, `mo query`, `mo replay`, `mo replay --fix`) its commands ran. A command run from a script the session wrote is not seen, so coverage can undercount.
- **Fix check**, recorded for a defect scenario and never part of correctness: the grader replays the scenario's retained failure from the session's starting data folder and checks the session's folder as the fix with `mo replay --fix`. It records the original's outcome, the verdict, and whether the folder was graded correct. A `verified` verdict can accompany a wrong fix, which is why the hidden tests alone judge correctness.
- **The original-first invariant.** A fix-check verdict of `verified` beside an original that did not reproduce is a violation of the rule that a fix is verified only after its original reproduced (section 8).

No grading rule changes after an answer is seen.

## 7. Missing and failed sessions

- A slot whose fixture check fails before the client starts is discarded (harness exit 3), the fixture is repaired, and the slot runs again. This is not a session.
- A slot whose client never produced an assistant event (harness exit 4) did not see its task. It runs again once, and `notes.md` says why.
- Every other session counts, including one that ran out of time, stopped, left no fix, gave no answer, or lost its record. Each is a miss.
- If a frozen client or model cannot run, collection stops for that client. No other model substitutes. The client's remaining slots are reported missing and count as misses toward the gate. Resuming with a different client or model is a new experiment.

## 8. The gate

The evaluation passes when all of these hold. Each is a count with point-estimate semantics; none claims a population rate.

1. At least 12 of the 16 sessions are correct.
2. At least 2 of the 4 sessions are correct in every scenario.
3. At least 5 of the 8 sessions are correct on each client.
4. No fix check breaks the original-first invariant.
5. The publication scan in section 10 passes.

**Demonstration**, reported and not part of the gate: an instrument counts as demonstrated on a client when at least one correct session on that client used it.

A pass supports Robert's release decision; it does not make one. A result below the gate leaves Phase 4 open, and the shortfall and every miss are published (phase plan R9).

## 9. Predictions

Written on 2026-11-07, before any scored session, by the agent that wrote this draft. They are checked against the results in `docs/status/phase4-run.md`, and a missed prediction stays recorded as missed.

| Scenario | Correct (of 4) | Instrument use predicted |
| --- | --- | --- |
| A | 3 | the committed history read through the operator log view or `mo history` in at least 3 |
| B | 4 | `mo fetch` and `mo query` in at most 2; the rest page through the author tools |
| C | 4 | `mo history` in at least 2, `mo replay` in at most 2 |
| D | 3 | `mo replay --fix` in at most 2 |

Overall: 14 of 16 correct, at least 7 of 8 on each client, no invariant violation, and the gate passes. Why: in the pilots every completed session succeeded with source access alone. The two defects here can also be found from source, as the cross-model review noted. A is the only scenario whose answer depends on data no author tool shows.

## 10. Freeze

Before the first scored session, `freeze.json` records:

- the git tree hashes of every build input: `src/`, `lib/`, `runtime/`, `vendor/`, `Makefile`, `ci/embed_docs.py`, `ci/embed_protocol.py`, `docs/agent-guide.md` and `docs/reference/README.md`, and the commit they come from;
- the candidate `mo`'s SHA-256 and version, built from that commit under `/opt/mo-phase4/candidate`;
- each client's path, version and SHA-256, and the models and settings of section 5;
- the SHA-256 of every harness and grading script (`learnability/phase4/*.py` and `learnability/wilson.py`), of `prompt.md` and of this protocol;
- the private repository's commit and the SHA-256 of every file under the four scenarios' folders;
- the round's built folders' `round.json`, and the slot order of section 4.

The round is built with the candidate (`harness.py build`), and `grade.py --check` runs against it before the freeze is recorded. At the start of collection, and again before a release tag, everything installed and every build input is compared with `freeze.json`; any difference stops the work.

## 11. Publication

Each session's `manifest.json` and `grade.json` are published to `learnability/phase4/evaluation/` through `publish.py`, which keeps the hidden tests' names private, after `scan.py` finds no session token or client credential in them.

The client streams and proxy logs go to an evidence release asset, following `evidence-learnability-2026-10`:

1. Stage the selected files in one folder and scan it.
2. Build the archive from that folder alone.
3. Extract the archive into an empty folder and scan the extracted copy, since the scanner refuses archives.
4. Upload only if both scans are clean, and record both scans' output.

After the round, the private repository's history is published, and its files are checked against the hashes in `freeze.json`. `docs/status/phase4-run.md` reports the setup, the predictions, the results by scenario and client, coverage and fix checks, every miss, the predictions against the results, and the actual result and boundary of each proof and check the phase relies on.

## 12. Budget

**No monetary spending cap.** Robert authorized proceeding without a monetary cap on 2026-11-07 in the continuation conversation for this milestone: “amount doesnt matter just do it”. The allocation remains 16 sessions of at most 60 minutes each, on the accounts the clients are logged in with. No session is dropped because it was costly.

## 13. Approval

Robert approved this protocol as drafted on 2026-11-07, in the Claude Code planning and implementation session for this milestone, with no change to sections 4, 5 or 8. In the continuation conversation on 2026-11-07, Robert authorized no monetary spending cap with “amount doesnt matter just do it”; section 12 records that decision. Collection still requires the recorded freeze and Robert’s authorization after reviewing it.
