# Live upgrade evaluation declaration

Status: declared before collection; artifact freeze is pending. The nightly
repair merged as 814ddd0f70e5fd613130fe87a9cd24cd187af8e6 in PR #317;
integrated repository verification is in progress. Collection is authorized by the implementation request;
Robert's explicit approval is required for release. No scored session has started. Development probes,
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
