# The Phase 4 evaluation protocol

**Historical protocol, approved by Robert on 2026-11-04.** Its section 9 predictions were written before any sessions and remain unchanged. The pilot campaign ended at 30 attempts; see [pilot findings](pilot/findings.md). No baseline or final round was collected.

**2026-11-05 amendment:** The mandatory baseline-before-implementation sequence below is superseded by R20–R24 of `docs/plans/2026-11-03-1704-feat-phase4-see-inside-plan.md`. This document preserves the original experiment and pilot rules; it does not authorize another 36-session round. A separately named diagnostic protocol must be approved before future scored collection. Pilots are not a measured baseline, and the original predictions are not rewritten to fit them.

The question is whether the Phase 4 instruments (the operator views, export with offline query,
and failure replay with fix checking) let fresh agents diagnose and fix a running Mo service's
bug more often than they can with the instruments Mo already has. It follows the Phase 2
protocol (`learnability/protocol.md`) for freezing, fresh sessions and keeping every result, and
differs where a running service needs it: the sessions operate a live service, a mechanical
oracle grades the fix, and the comparison is between two rounds.

## 1. Rounds

| Round | Instruments | Counts toward the comparison |
| --- | --- | --- |
| Pilot | the baseline instruments | no; kept in `learnability/phase4/pilot/` |
| Baseline (round 1) | the baseline instruments | yes |
| Final (round 2) | the baseline instruments and every Phase 4 instrument | yes |

Pilots choose and harden scenarios, prompts and the grader, under predictions recorded before
each pilot. After the last pilot, everything in section 7 is frozen and the baseline runs as
fresh sessions. No pilot session enters the baseline. A baseline is never rerun, hardened or
replaced because its rate is inconvenient. A change to anything frozen after a baseline session
has seen its task starts a new, separately named experiment with its own new baseline; the old
one stays recorded as it was.

## 2. Scenarios

Each scenario is a **variant** of the task queue: a copy of `services/task-queue/` with one seeded
defect, kept in `learnability/phase4/variants/NAME/`, never in `services/`. A variant starts from
a known data folder that a fixture script builds, receives known traffic at known times before
the session starts, and is running when the session begins. Three scenario families, two
variants each, so that each promise is exercised and no single bug decides a family:

| Family | What the defect does | The instrument it is meant to need |
| --- | --- | --- |
| Wrong state | A handler returns a plausible but wrong new state with no failure (for example, `expire` leaves leases past their time, or `fail` counts an attempt twice) | live views of state and the log since the snapshot |
| A question the tools cannot answer | The prompt asks something about the state that no author tool reports (for example, which payloads were leased more than once, or the id the next job will get), and the fix needs that answer | export and an offline query |
| Recorded failure | A handler fails only on a particular state and argument (a defect: `stop`, an overflow, a bad index), or exceeds its work budget on the seeded state (a quadratic path) | the failure record, replay and the fix check |

The work-budget variant matters because today a work-limit failure prints a `failure:` line and
no report at all. No scenario depends on live memory pressure: the design does not reconstruct
the mailbox (`docs/design/introspection.md`, section 1), so memory-pressure cases are regression
tests for the implementation, not evaluation scenarios.

Pilots may replace any of these with others in the same family. The frozen list is in the
baseline's `freeze.md`.

## 3. Sessions

- **Fresh**, as in the Phase 2 protocol: no memory, no earlier conversation, no project
  instructions, one scenario per session.
- **Where sessions run.** On the same machine as the running variant. The session's working
  folder holds the variant's source and nothing else from this repository. It can run the pinned
  `mo`, read the service's standard error log (a file the fixture writes), and reach the service
  at `127.0.0.1`. It cannot read the fixture scripts, the grader or other variants. This meets
  the design's requirement that a full workflow have host access (`introspection.md`, section 2).
- **The baseline instruments**: the variant's source, the pinned `mo` (`check`, `test`, `run`,
  `serve`, `guide`), the author tools over MCP, the service's standard error log, and a
  read-only copy of the data folder taken when the session starts.
- **The final instruments**: the same, plus the operator tools on the same endpoint and the
  offline commands, from the `mo` that ships them. The guide (`mo guide`) is how a session learns
  them; the prompt does not teach them.
- **The baseline `mo`** is built from a commit with no Phase 4 instrument, named in `freeze.md`
  with its SHA-256. The final round's `mo` is the release candidate. The grader uses each
  session's own `mo`.
- **Credentials.** Each session's service gets a fresh random token in an environment variable
  the client reads. It is never printed, and section 8 scans for it before anything is published.
- **Time.** Each session has a wall-clock limit, frozen at baseline (60 minutes is proposed).

## 4. Clients and recording

Two families, each with an exact client and model, as in Phase 2. The client probe
(`learnability/phase4/preflight/README.md`, "Client names and approvals") ran both native
command-line clients against a recording proxy:

| Family | Client | Model, as the client reported it | MCP configuration | Unattended approvals |
| --- | --- | --- | --- | --- |
| Anthropic | Claude Code 2.1.317, `claude -p` with `--mcp-config FILE --strict-mcp-config --output-format stream-json --verbose` | `claude-opus-5-5` (it also used `claude-haiku-5-5` for side work) | an `http` server with an `Authorization` header from `${TOKEN}` | `--allowedTools 'mcp__NAME__*'` |
| OpenAI | Codex 0.166.0, `codex exec --json --ephemeral` | `gpt-6.1-sol`, reasoning `xhigh`, from `~/.codex/config.toml` | `mcp_servers.NAME.url` and `bearer_token_env_var` | `mcp_servers.NAME.default_tools_approval_mode="approve"` |

Without the last column, Codex refuses every unannotated author tool under
`approval_policy="never"`, which would cripple the baseline and not the final round. Both rounds
use the same approval settings.

Earlier rounds ran as Amp threads. Amp is not installed on the probe machine, so its MCP naming,
approvals and transcripts are unverified. Robert chose the native clients above at approval;
moving to Amp would need the same probe run through it first.

**Recording.** Two records per session, used identically in both rounds:

1. the client's own event stream (`stream-json`, `--json`), kept whole;
2. a recording proxy between client and service (the probe's, hardened in the pilots), which
   writes every request and response in order with its status: initialize, list, every call and
   its result or error.

A session's record is complete only if every tool call in the client stream has its request and
response in the proxy log, and the reverse. An incomplete record is a protocol failure of that
session (section 6), not a reason to rerun it.

**Model versions.** Neither client's model name is shown to be immutable: `claude-opus-5-5` and
`gpt-6.1-sol` are names the providers serve, and nothing here verifies that the same name serves
the same weights on another day. `freeze.md` records the names, the client versions, every
setting the client exposes, and the date of each session, and that is the extent of the claim.
If a frozen name or client version cannot be run when the final round starts, that family's
comparison is not made and its baseline is reported alone; no other model substitutes for it.
If neither family can run, the comparison ends and a new experiment, with a new baseline, is
declared.

## 5. Grading

**The primary measure** is a session's **success**: the fix it leaves in its working folder, run
by the grader, passes every check below. The analysis unit is the session.

1. `mo check` of the revised program is clean, with that session's `mo`.
2. The scenario's hidden tests pass: a hidden test module, copied next to the revised source,
   uses the Phase 3 service simulation (`Simulation.start`, `Simulation.send`) to start the
   service from the scenario's known pre-state and send the known triggering message at its
   known arrival time. It checks the correct reply and new state, which the scenario's author
   writes from the specification, not from any agent's output.
3. The task queue's own tests (`services/task-queue/queue-tests.mo` and `job-queue.mo`'s
   examples) still pass on the revised program.
4. For the question family, the answer in the session's final message equals the true answer the
   fixture computed. The grader reads only that value, by a frozen rule.

The grader needs nothing from Phase 4: the simulation, `mo check` and `mo test` exist today. Its
source, scoring rules and the commit it runs from are frozen with the baseline. The final round's
fix checker (`mo replay --fix`) is cross-checked against it on every final-round session where
both apply: a session the oracle grades as a success whose replay verdict is not `verified`, or
the reverse, is recorded as a finding against one of them.

**Secondary, descriptive measures**, never part of the comparison: a diagnosis grade from two
blind model graders, as in Phase 2 section 4 (whether the final message names the cause), and
**instrument use**: the share of sessions that called each operator tool and offline command.

## 6. Missing and failed sessions

- A session that failed before it saw its task (the client did not start, the provider was down)
  is run again, and `notes.md` says why. That is the only rerun.
- A session that saw its task and then ran out of time, stopped, left no fix, lost its record, or
  wandered off the task counts as a failure.
- A session whose service variant did not reach its seeded condition before the session started
  (the fixture check failed) is not a session: it is discarded before the agent starts, and the
  fixture is repaired. The check runs before every session, in both rounds.

## 7. Freezing

`learnability/phase4-round-1/freeze.md` records, before the first baseline session, everything
in the Phase 2 protocol's section 1, and also: each variant's source and fixture by SHA-256;
the hidden tests and the grader; the prompt; client versions, models and every setting of section
4; the approval settings; the session time limit; the allocation; the analysis of section 8; and
this protocol by SHA-256. The final round's `freeze.md` repeats it, changing only the `mo` binary
and adding the instruments' guide hash.

**Allocation.** 36 sessions per round: 3 families × 2 client families × 6 sessions, each variant
getting 3 sessions per client family. Sessions run in a random order, from a seed recorded in
`freeze.md`. The final round uses exactly the same allocation.

## 8. The comparison

The primary result is the difference between the final and baseline success rates, pooled over
every scenario and both client families, with its 95% Newcombe hybrid score interval
(`learnability/phase4/compare.py`; method 10 of Newcombe 1998, tested against that paper's
examples in `test_compare.py`).

- **Improved** when the interval's lower bound is above zero: one-sided at 2.5%.
- **Not shown** otherwise. Phase 4 then stays open, the shortfall is recorded as measured, and
  the work it points to is the next work.

There is one primary comparison, so no multiplicity correction is applied to it. Per-family and
per-client rates are reported with their Wilson intervals (`learnability/wilson.py`) and are
descriptive only.

**Size.** The comparison pools every session, but the families' true rates differ, so each
round's successes are a sum of one binomial per family rather than one binomial. `compare.py
--strata` computes the exact power for that sum; `compare.py --power` assumes one rate for every
session. Both, for this protocol's allocation:

| Sessions per round | Assumed true rates, baseline to final | Power |
| --- | --- | --- |
| 36 | the section 9 predictions per family: 50% to 80%, 10% to 70%, 40% to 75% (`--strata 12:50%:80% 12:10%:70% 12:40%:75%`) | 0.96 |
| 24 | the same predictions, 8 sessions per family | 0.88 |
| 36 | 35% to 75% in every family | 0.94 |
| 36 | 35% to 65% in every family | 0.72 |
| 36 | 50% to 80% in every family | 0.79 |
| 24 | 35% to 65% in every family | 0.59 |

24 sessions (a multiple of 12, so every variant gets the same sessions from each client) already
give more than 80% power if the predictions hold. Robert chose 36, as margin: if the
true improvement is smaller than predicted, such as 35% to 65%, 36 keeps 0.72 where 24 would keep
0.59. Power is not monotone in the number of sessions for a discrete test, so the frozen number is
fixed in advance and its exact power reported, rather than searched for after the fact.

**Room.** After the baseline, `compare.py --room K1 36 36` gives the fewest final successes that
would show an improvement. With 36 sessions, a baseline of 32 or more successes leaves no room at
all. If the baseline leaves no room, or would need 35 or 36 of 36, the improvement experiment
stops: the baseline is recorded honestly, and either Phase 4's evidence comes from a newly
declared experiment with harder scenarios and its own fresh baseline, or Robert decides how the
release is judged. The baseline is never replaced.

## 9. Predictions

Written on 2026-11-04 by Claude, in the design step, before any pilot. They are the only
predictions: Robert decided on 2026-11-04 not to add his own. They are not edited after.

| Family | Baseline | Final | Why |
| --- | --- | --- | --- |
| Wrong state | 50% | 80% | The author `list` tool already shows jobs, so a careful agent can find wrong leases; the views add the log since the snapshot, which shows which call did it |
| A question the tools cannot answer | 10% | 70% | Without an export, the answer needs decoding SQLite records by hand; with it, a short query |
| Recorded failure | 40% | 75% | The defect report names the location and top-frame variables, but not the state and argument that caused it; the work-limit variant has no report at all today |
| **All** | **33%** | **75%** | |

Instrument use in the final round: at least one operator tool in 90% of sessions; an offline
query in at least 60% of question sessions; replay in at least 60% of failure sessions.

## 10. Publication

Before anything from any round is published, a scanner reads the exact files to be published:
transcripts, client event streams, proxy logs, notes and their rendered forms. It fails if it
finds any session's token, in plain text or in any encoding the transports use (base64,
base64url, URL encoding, JSON escapes, hexadecimal), or if it cannot read a file it was given.
It is tested with seeded leaks in each encoding before its first use. An expired token is still a
leak. Unredacted originals stay private; the published copies keep everything grading and
reproduction need. Building the scanner is part of the baseline milestone.

## 11. Where results go

`learnability/phase4/pilot/` for every pilot attempt; `learnability/phase4-round-1/` and
`learnability/phase4-round-2/` with `freeze.md`, the prompt, each session's grading, `results.md`
and `notes.md`, as Phase 2 rounds do; `docs/status/phase4-run.md` for the predictions against
the results. Client streams and proxy logs go to the evidence release after the scan, not into
git.
