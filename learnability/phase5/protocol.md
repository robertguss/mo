# Live upgrade evaluation declaration

Status: original declaration retained below; the credential correction declares
experiment 2 before its collection, and the last section declares experiment 4
before its collection. The nightly
repair merged as 814ddd0f70e5fd613130fe87a9cd24cd187af8e6 in PR #317;
integrated repository verification passed (the earlier interrupted run is retained). Collection is authorized by the implementation request;
Robert's explicit approval is required for release. One experiment-1 session was interrupted for an instrument defect; at the
experiment-2 declaration none of its sessions has started. Development probes,
stub clients, preflight and instrument repairs never enter the denominator.
This is a capability demonstration, not a comparison or population-rate claim.

## Scenario and allocation

Four fresh sessions, in order Codex, Claude, Codex, Claude, each with a fresh
populated task queue. Each must add a persistent revision field initialized by a
pure migration while preserving jobs, payloads, attempts and leases, and expose
the revision through a tool. Each must run `mo push` through the recording proxy
and answer the resulting revision. The hidden grader checks the running service,
its durable activation and a folder-only restart, not merely the edited source.
The prompt is `evaluation/prompt.txt`; fixture, collector and hidden checks are
in `evaluation/`. Their hashes are frozen before collection. The migration must
be named `migrate`, the persisted Int field and read-only tool `revision`, and
the final stripped assistant message must be exactly the observed value `1`.
The grader independently checks persisted fields, jobs and next_id; a constant
tool without the stored field is a miss.

Each session gets the frozen executable, editable candidate source, read-only
historical source (MOSB/MOCC if required), guide, service log, proxy URL and
names of two distinct credential environment variables. No active-bundle read
tool is added. Rehearsal is optional and never supplies an activation ordinal.
Reuse the Phase 4 separate session user, neutral service command line, protected
fixture/grader directories, fresh home, process cleanup and private artifact store.
Do not expose a writable live data folder or the direct service port to the client.

Freeze Codex 0.166.0 with `gpt-6.1-sol`, `xhigh`, ephemeral JSON output, web off,
never approvals and explicit MCP tool approvals; Claude Code 2.1.317 with
`claude-opus-5-5`, strict MCP config, stream JSON, Bash and server tools allowed,
no persistence and no self-update. Hash actual binaries before collection. The
prior discovery probe's Claude client automatically fell back to `claude-opus-4-8`.
Retain actual model/fallback events: do not call that an Opus 5.5-only result.
No operator-selected model substitution is authorized. A client stream must
report the requested model; missing or substituted model evidence is a miss.
Retain auxiliary model events as well. A fallback
or unavailable frozen model leaves the affected slot a miss under this protocol.
Limit each launched session to 3,600 seconds, including initialization stalls.

## Fixture preparation

Before collection, a temporary fixture-only author tool leases through the ordinary
30-minute lease function using a fixed 2099 time. One tool-only activation then
restores the exact ordinary queue source. This keeps deadlines stable across the
four sessions without changing production lease behavior or immutable queue tests.
The protected expected record pins all stored jobs, next_id 5, requested stored
revision 1 and the exact preparation history prefix. The session must add exactly
one activation after that prefix; preparation is never credited to the agent.
No historical migration is introduced, so the supplied original source suffices;
no active-bundle read tool or undisclosed context is required.

## Gate and records

At least three of four slots must pass, including at least one on each client.
All four slots must be present and correctly attributed; duplicate slots are a
protocol failure. Missing, stalled, timed-out, refused and incompletely recorded
sessions remain misses. No replacement after seeing a task. A fixture failure
before launch is recorded outside the denominator and repaired before collection.
A clean publication scan is required. Below gate means phase open; passing does
not authorize a release. `evidence.py gate RECORD.json` aggregates the four frozen
slots, with `sessions` and `publication_scan_clean` keys. It trusts the recorded
hidden verdict and scan result; it does not produce either of them.

Before collection, prove the hidden grader accepts a reference migration and
rejects the unchanged service, initializer substitution, dropped job, altered
payload, duplicate completion, wrong revision and source-only fix without push.
Keep graders and expected values inaccessible to sessions. Restore immutable
queue tests before grading source. Inspect live tool answers and stopped-folder
history with the frozen binary; reconcile accepted calls and activation identity.

Reuse Phase 4 proxy and scanner only after characterization with both credentials.
Headers must be forwarded, while neither bearer value may enter recorded headers,
body strings, stdout or stderr. Scan against both service credentials and client
secrets, including encoded forms. The existing proxy records JSON bodies and
response errors; its stream-to-tool matcher needs an explicit command-origin
category for upload calls made by `mo push`. Do not silently exempt unmatched
calls or treat shell upload calls as native MCP calls. The raw TCP tunnel keeps
Host unchanged; use matching ports or an explicitly allowed authority. Synthetic
empty-argument probes establish routing, not credential refusal or activation.

Retain full private streams, proxy request/reply pairs, source edits, service log,
start/end folder copies, hidden grades, client/model events, exit statuses and
SHA-256 manifests. Publish every slot and miss. Stage curated artifacts, scan,
archive, extract to an empty directory, scan again, then let the parent publish.
Curated session evidence is prepared for the requested PR; no release publication
or deployment is authorized. `evaluation/publish.py` stages, scans, archives,
extracts and rescans all four slots, keeping private originals intact.

## Freeze and collection prerequisite

Record the merged nightly-repair commit, its ancestry on main and verification
results. The starting main was d31c9301132ac9e55432bf9e36855c2914ecab21;
no unmerged repair is an acceptable input. Then freeze candidate commit, exact
build commands/compiler versions, binary hashes, build inputs, protocol, prompt,
harness, proxy, scanner, grader, fixtures, hidden controls and slot order. Compare
all installed artifacts before collection and again before any release decision.
`evidence.py build-inputs ROOT` records tracked build input file bytes; it is not
a complete experiment freeze or an attestation of a clean tree. Require a clean
tracked tree and inspect untracked build inputs separately. Any frozen-input
change after collection begins requires a separately named experiment retaining
all prior results.

## Credential correction and separately declared experiment

At 2026-11-08T23:28:51.391236+00:00, experiment 1 was stopped after its first slot launched.
The collector configured the native MCP upgrade header as a raw secret rather
than `Bearer VALUE`. The CLI push credential environment was correct, but the
native tool setup violated the supplied-credential contract. Detection preceded
inspection of the slot outcome. The actual transcript and interruption remain;
slots 2–4 were not launched and remain missing misses in that experiment's
four-slot record. This is an instrument failure, not evidence of agent inability.
No attempt is relabeled development or silently replaced.

Experiment 2 is declared now, before its collection: four fresh slots in the
same Codex/Claude/Codex/Claude order, identical scenario, frozen clients/models,
3,600-second limit, grading rules and 3-of-4 gate with at least one per client.
It uses a separately frozen collector: Codex maps a derived Bearer-form
environment value to the header, Claude expands `Bearer ${QUEUE_UPGRADE}`,
and `mo push` retains the original raw credential variable. Real-service valid
argument controls distinguish missing scheme E1414 from authenticated stale
base E1416. The OS stub also checks both supplied client configurations.

The prior pause/drill inputs and binaries are unchanged. Preserve their original
freeze and verify the relevant input hashes before carrying those results
forward; do not repeat or select latency samples to improve the margin. Publish
both experiment records and every launched session/miss, with distinct
denominators. The missing-model-evidence rule remains unchanged.

## Collection outcome and retained grading analysis

Experiment 2 completed all four original frozen slots: 0/4, gate missed. No
replacement sessions were collected. The original manifests, transcripts and
publication scans are under `results/experiment-2-original/`; the interrupted
experiment remains under `results/experiment-1/`. Phase 5 remains open.

A separately named `experiment-2-qualified-grader-analysis` was declared before
regrading. It repairs the collector's assumption that the declared leaf name
`migrate` means `Main.migrate`. All four client sessions used the unchanged
original collector. The analysis compiles the accepted report's qualified
selector against retained original source and independently checks identities.
An accepted report alone does not attest a push: unmatched proxy calls and
missing model evidence remain misses. Original verdicts remain published beside
the analysis; no gate or denominator changes. The declaration and exact analysis
input hashes are retained in results.

## Experiment 4: outcome-gated declaration

Experiment 4 is declared here, before any of its sessions launches. Experiments 1,
2 and 3 and every development failure stay published unchanged; experiment 3 is
a stopped experiment and its slot is not regraded.

The pass conditions are narrowed from experiments 2 and 3. None of the recorded
misses showed an upgrade that did not work: in all five scored sessions that ran
to the end, the service state and the recorded activation were correct. The
misses came from the evidence of how bytes travelled, not from the work. After
experiment 2 the collector gained a syscall tracer that had to recognise every
action of two closed-source clients on a live kernel and rejected anything it did
not recognise. Each new client behaviour needed a repair, and because the counting
rules forbid repairs during an experiment, each surprise cost a whole experiment;
experiment 3 stopped after one session on resumed `sendfile` calls. This is a
capability demonstration, so experiment 4 judges the result of the upgrade.

Experiment 4 keeps experiment 2's shape: four fresh sessions in Codex, Claude,
Codex, Claude order, the frozen clients and requested models above, the same task
and the same prompt, a 3,600-second limit, and a gate of at least three passes with
at least one per client. The prompt is byte-identical and still names `mo push`.
Grading credits an upload through the proxy whether the agent used `mo push` or
the client's native MCP route, because the service gives every way in one upgrade
path. A session passes when all of these hold:

- the hidden checks on the running service pass (`task_state`);
- the folder-only restart check passes (`task_state`);
- exactly one durable activation with the expected identity follows the fixture's
  preparation, and the proxy record holds the accepted commit reply for it
  (`activation_identity`); the session account cannot reach the service's direct
  port, so an accepted commit can only have arrived through the proxy;
- the immutable source tests pass (`source_tests`);
- the final message, trimmed of whitespace, is exactly the revision value the grader
  itself reads from the live service (`final_answer`).

Command origin by process trace, traffic completeness and model provenance are not
pass conditions, and the syscall tracer does not run. Each session's record states,
without grading them, the requested model, the models the client's own output
reports (or that it reported none), whether the transcript shows `mo push`, how
many uploads had a matching native client event and how many did not, and the
number of proxy calls with no matching client event. A session whose client reports
a different model from the one requested counts when the conditions above hold and
is published under the reported model; this replaces the earlier rule that missing
or substituted model evidence is a miss.

Codex runs with `--sandbox danger-full-access` under the collector's isolation (a
separate session account, a firewall on the service port, read-only grader inputs),
with the HTTP Responses transport and diagnostic logging that let it report a model.
Experiment 2 used the `workspace-write` sandbox; every development and scored
session since then used full access.

Experiment 2's counting rules carry over unchanged: every launched session counts,
there are no replacements, and neither the collector nor the grader changes after
the first launch. A harness defect seen after a launch is published with the affected
slot as a miss. If the gate is missed, the phase stays open, the miss is published,
and no further experiment runs without Robert's decision. Before the freeze, one
development session per client must pass these conditions live on the final
collector with no offline repair, and the grader must accept a reference upgrade and
reject each wrong outcome listed under Gate and records. An independent review of
the freeze and Robert's approval both precede the first scored launch. The
declaration record is `results/experiment-4/declaration.json`.
