---
title: Phase 4, Agents Can See Inside It - Plan
type: feat
date: 2026-11-03
topic: phase4-see-inside
artifact_contract: ce-unified-plan/v1
product_contract_source: ce-brainstorm, plan-audit, user discussion
execution: code
---

# Phase 4, Agents Can See Inside It - Plan

## Goal Capsule

- **Objective:** An agent operating a running Mo service can inspect it, answer questions through an offline query over exported state, reproduce a recorded handler failure, and check a proposed fix against the reconstructed case. Fresh-agent evaluation supplies the evidence for releasing Mo v0.1.
- **Execution:** Six ordered units: design and feasibility, pilots and independent baseline, operator tools and export, failure recording, offline commands, final evaluation and release. This is the single implementation plan. Each unit extends the phase design document with its detailed decisions and evidence; a second brainstorm or implementation plan is not a prerequisite.
- **Authority:** The language reference, architecture, implementation rules, services design and `AGENTS.md` govern implementation. This revision incorporates the audit and subsequent user decisions. New behavior is specified below; it is not claimed to exist already.
- **Ready to start:** U1. Later units have explicit evidence gates. Unmeasured export size, client logging, model availability and history sufficiency remain investigations with owners, not verified dependencies.
- **Stop and report:** If a required export cannot fit the agreed process budget; the intended agent cannot reach the host; comparable evaluation configurations cannot be frozen; the independent baseline leaves the predeclared comparison infeasible; a seeded reproducible failure cannot be replayed; or implementation requires a store-version or language-rule change beyond the clarification below. Resolve the failed gate in this plan and the design document before dependent work proceeds.
- **Finishing:** Implementers open milestone PRs using the repository template and required check results. Robert approves the phase design, runs the evaluation sessions and decides the release. Repository review and merge rules remain those in `AGENTS.md`.

## Revision and Evidence Status

This revision incorporates PA-1 through PA-9 from the audit (`2026-11-03-1704-feat-phase4-see-inside-plan.audit.md`, removed from the repository on 2026-11-04 and kept in git history), the user's decisions on capture, replay, compatibility and evaluation, and the request for continued Lean proofs with TLA+ where useful. Requirements and acceptance examples below are the implementation contract; implementers do not need to assemble decisions from the audit conversation.

The original plan described several decisions as previously approved without linking the originating sessions. That provenance remains unverified; those labels are removed. The current discussion explicitly authorizes this consolidation and settles the changes recorded here. It does not establish historical approvals, external prototypes, measurements or completed proofs.

The six-unit order and three product promises remain. Changes include best-effort diagnostic capture, narrower fix-verification claims, separate identity and schema checks, pilot/baseline separation, explicit preflight investigations, and formal-verification deliverables. Names such as `mo fetch`, `mo replay`, `mo query` and `mo history` remain working command names until U1 fixes their CLI contract.

## Product Contract

### Summary and Problem

Phase 3 supplies a pure service runtime, stable event lines and defect reports. Phase 4 adds authenticated operator views, bounded exports, separately retained failure diagnostics, and offline commands. Evaluation determines what these instruments let fresh agents accomplish beyond the existing tools.

A pure handler's logical inputs are its state, arrival time and arguments. Reproducing execution also requires the original program, runtime semantics and relevant limits. Recording a heap ceiling does not reconstruct the live heap's occupants: the mailbox and other omitted allocations can make an offline execution succeed where the live execution failed. The phase reports that distinction and never turns an unreproduced original failure into a verified fix.

### Shared Decisions

- All three promises ship: live views, offline queries, and failure replay. Evaluation and the release remain part of the phase.
- Operator tools use the existing `/mcp` endpoint and the service's existing token. Any holder of that token can inspect the service; there is no additional role or operator credential.
- Caller-written queries run outside the service. The service exports bounded data; it does not execute arbitrary caller code.
- Diagnostics are separate from application state and the recovery log. A failed read-only call may create a diagnostic record, while its state and recovery position remain unchanged. U1 clarifies reference §11.5 to express this agreed interpretation.
- Capture is best effort with explicit missing-record reporting. Continuation depends on ordinary persistence remaining safe; an unusable store follows the existing store-failure stop policy.
- Fix verification means the original failure reproduced and a compatible revised handler succeeded in the reconstructed offline environment. It establishes that case, not fitness under omitted live memory pressure or all future workloads.
- Pilots select and harden scenarios. A fresh baseline follows the freeze and is never selected for a conveniently low observed success rate.
- Lean remains the primary semantic proof system. Small TLA+ models examine protocol ordering; CBMC and implementation tests examine critical C behavior. Each claim states its assumptions and implementation connection.

### Requirements

**Phase shape**

- R1. Execute U1 through U6 in order, advancing only when the preceding unit's Definition of Done is met.
- R2. Maintain one phase design document, `docs/design/introspection.md`, extended with the decisions and evidence each unit produces.
- R3. Use this document as the single implementation plan. Record detailed interfaces, formats, limits and experiments in the phase design document and link them from the relevant unit.
- R4. U1 closes on approved design and successful preflights; U2 on the frozen independent baseline and its protocol gate; U3–U5 on their implementation and verification gates; U6 on evaluation and the release decision.

**The operator door**

- R5. A running service answers built-in operator tools beside author-declared tools on its existing MCP endpoint.
- R6. Views cover processes, current state, mailbox, pending timers, memory use and peak, supervisor failures/restarts/stops, retained failures, and committed messages since the last snapshot. Any separately retained older history is an explicit U1 decision.
- R7. Export the current state or one retained failure, including its pre-call state, message and arrival time. Artifacts carry separately defined format, runtime-build, compiled-program and schema identities.
- R8. Wrong or missing authentication gets the same refusal as author tools. Fetch sends the credential only to a validated loopback peer.
- R9. Operator tools, exports, diagnostic records and reports never include the runtime's stored bearer token. State and arguments remain author data and are shown as such. Evaluation publication separately checks artifacts for its known credentials.
- R10. Operator calls do not change application state, enter its recovery log, enqueue messages or affect supervisor counts. Pin storage and temporary inspection work have independent named budgets and do not retain live values into later handler execution.

**The failure record**

- R11. Attempt to capture every handler failure the supervisor counts, for tools (including read-only tools) and timers. Record the pre-call state, arguments, time, sequence, outcome signature and budgets. Application `Error` replies and admission refusals are not handler failures. If preparation, encoding or storage fails, report an unavailable record with a bounded reason; do not silently claim it was saved or retry indefinitely. Continue only if ordinary persistence remains safe.
- R12. Retained failures and capture refusals are visible in operator views. Preserve the three stable `mo serve:` event-line shapes; place a record id or unavailable-record reason in the following indented report.
- R13. Bound retention by both count and total bytes, with a deterministic rule that prevents one repeatedly failing timer from evicting the first failure of another kind. Atomic insertion and eviction never expose a partial failure.

**`mo` over the files**

- R14. Replay a saved failure from an export or stopped data folder using its original compiled program and runtime build. Report the original message, pre-call state, stack spans and available stopping-frame variables, with explicit unavailable/truncated diagnostic fields.
- R15. Classify reproduction using a structured signature: a handler defect matches its reason and source location in the original program; a resource failure matches the specific limit under the recorded budget. Rendered report text need not match byte-for-byte. Distinguish reproduced, different outcome, not reproduced, and refused; identify omitted live-memory conditions where applicable.
- R16. Run a pure Mo query over a state export or over a saved failure's state, time and ordered arguments. A query entry may not require capabilities, including through nested types or captures. A fix-check mode uses the same command machinery but also runs the original handler first and enforces R19.
- R17. History reads a stopped service's data folder without starting a service or running its timers. It shows persisted log/diagnostic data and explicitly identifies retention boundaries; it does not invent lost in-memory supervisor events.
- R18. Apply command-specific compatibility checks from KTD5. Exact replay requires original compiled-program and runtime-build identity. A state query permits added functions while requiring the same state schema. A failure query/fix check also requires compatible argument types, count and order, and a valid callable/result contract. Refuse schema changes with an explanation; migration is outside v0.1.
- R19. History and state queries have named work/memory limits. Replay and failure queries use recorded budgets; revised updating handlers also pass the saved-state size check. A fix is verified only after original-failure reproduction and successful compatible revision under the reconstructed conditions. A passing revision with an unreproduced original remains unverified, even if the same heap ceiling was configured.

**The evaluation**

- R20. U1 specifies the primary success measure, grading oracle, between-round comparison, predicted effect, uncertainty rule, session allocation and failure/missing-data rules. Derive sample size for that comparison. `learnability/wilson.py --size` may help choose single-rate interval widths; it is not a two-sample power calculation.
- R21. Within U2, use pilots to select/harden scenarios, then freeze scenarios, prompts, grading, client modes, model versions/settings and session counts before collecting a fresh independent baseline using only existing instruments. Preserve pilots but exclude them from the final comparison. Do not harden and replace a frozen baseline because its observed rate is inconvenient.
- R22. U6 repeats the frozen scenarios on the same supported model configurations and client modes with Phase 4 instruments available. Apply the predeclared comparison. A miss stays recorded and becomes further work; model unavailability invokes the frozen replacement/rebaseline policy rather than an undisclosed substitution.
- R23. Keep seeded defects and work-budget failures in task-queue variants separate from the served program. Keep prompts, ordered transcripts, grading evidence and failures from every round. Memory-pressure cases are implementation regressions unless U1 establishes a reproducible evaluation case.
- R24. Scenarios collectively exercise live views, a query author tools cannot answer, and failure replay. Instrument use is a separate coverage measure; it does not change the diagnosis/fix success denominator between rounds.

**Release and verification**

- R25. Release v0.1 only after the final evaluation meets its declared gate and Robert decides to release. Update status, installation instructions, promises and the roadmap to match the evidence.
- R26. Each unit updates the agent guide alongside the commands/tools it ships; guide checks exercise its claims.
- R27. Add commands, tools and documentation without new language semantics. Clarify the existing read-only rule in §11.5 as above and update §13.4/§15 as appropriate. Any further language-rule change or store `user_version` change requires revisiting scope.
- R28. Deliver the Lean properties, TLA+ models, relevant C proofs and implementation checks assigned in the Verification Design. Report their boundaries and actual results; absent tooling, waived obligations and excluded cases are not passing evidence.

### Actors and Flows

- **Operating agent:** Has the necessary source/compiled program, supported runtime build and token. Full export/offline operation requires local execution or host access such as SSH. This is a deployment prerequisite to demonstrate in U1, not an established fact about all remote agents.
- **Service:** Runs locally or behind a proxy. Proxy-only clients may use authenticated views; loopback fetch requires access to the host. No TLS client is added in this phase.
- **Evaluation agents:** Fresh sessions from at least two model families, with the frozen environment and only the round's permitted instruments.
- **Robert:** Approves the design, runs the evaluation sessions and decides release.

- F1. **Live diagnosis:** Inspect views, export/query state, export a retained failure, replay the original, revise the handler, then request a fix check with both the original and revised program. A logical wrong reply is investigated through views/queries; it does not become a recorded runtime failure.
- F2. **Stopped service:** Read persisted history and failures, replay the original, check the revision, then follow the existing fresh-folder procedure for starting changed code. A missing diagnostic remains unavailable; the CLI does not fabricate replay inputs.
- F3. **New question:** Export state, write a pure function in the defining module or through its exposed interface, and query that snapshot. Normal source privacy remains in force.
- F4. **Evaluation:** Run pilots, freeze, collect the independent baseline, implement instruments, run the final round, and apply the frozen grading/comparison rule with all failures retained.

### Acceptance Examples

- AE1 (R8). Wrong or missing tokens receive the author-tool refusal. A non-loopback destination, redirect or proxy configuration cannot receive the fetch credential.
- AE2 (R11). A handler defect records state, arguments, time, sequence, signature and budgets when capture succeeds. An application `Error` records nothing. Preparation or write failure reports the record unavailable; only an unsafe persistence state triggers the existing store-failure policy.
- AE3 (R10). Calling every operator tool leaves application state, recovery position, mailbox and supervisor counts unchanged, including when the mailbox is full.
- AE4 (R14–R15). Original-program replay matches the structured defect reason/location or specific limit. Stack/variables are diagnostic evidence, with any capture truncation named. A different failure is not reproduction.
- AE5 (R18). Adding a state-query function preserves compatibility; changing the state schema is refused. Failure queries additionally reject incompatible arguments, reordered parameters or an invalid handler-result contract.
- AE6 (R17). History on a stopped folder shows retained persisted data without starting the service. A live owner's lock is refused; a hot journal follows the explicit recovery-permission contract.
- AE7 (R16). A query requiring a capability through its parameters or captures is refused by the query-entry checker.
- AE8 (R22). A final result below the frozen gate leaves Phase 4 open and preserves the shortfall.
- AE9 (R19). A reproduced original defect followed by a compatible successful revision verifies the reconstructed case. A revision that still exceeds the recorded work budget fails. An unchanged handler that failed with a populated live mailbox but succeeds offline never becomes a verified fix.
- AE10 (R7). Pages from two different export generations at the same log sequence cannot be assembled as one accepted export; restart, expiry and replacement invalidate stale requests.
- AE11 (R6). A service with private nominal state, only a scalar read-only tool, and a timer supports state inspection/export without depending on the author-tool MCP shape closure.
- AE12 (R11). A failed read-only call may create a diagnostic record while leaving state/recovery log unchanged. Exact-limit arguments and an exhausted live heap produce either a complete bounded record or an explicit capture refusal.
- AE13 (R20–R24). Pilot observations do not enter the baseline denominator. Changing a frozen scenario starts a new declared experiment rather than overwriting the old baseline.

### Scope Boundaries

Deferred: in-process arbitrary queries; replay of a successful committed message by log position; separate operator credentials; capability-effect recording; profiling views; live upgrades/migrations; multi-core scheduling; dashboards; contribution policy. Historical data beyond the current snapshot requires U1 evidence and a separate compatible archive if selected; never retain pre-snapshot rows in the recovery log, whose readers expect a post-snapshot tail.

The three parked Phase 3 operational items are included only if a unit demonstrably needs them. Safe-c library adoption, a runtime rewrite, TLAPS and GenMC are not prerequisites for Phase 4. Verification techniques from safe-c inform the work below; any selected library pilot must have a narrow scope and explicit acceptance evidence.

## Dependency and Decision Gates

No external evidence pointers were supplied during the audit discussion for prior approvals, frozen model configurations, transcript completeness or remote host access. No maximum-export measurement, imported-query prototype or history-sufficiency result is assumed. The following work is assigned and must close before its dependent implementation or evaluation freezes.

| Gate / audit entries | Owner and required evidence | Blocks |
| --- | --- | --- |
| Capture resources and transaction outcomes (L11, PA-2) | U1 designs byte/scratch accounting and clean-refusal vs unsafe-store classification; U4 verifies preparation, rollback, uncertain commit and write faults. | U4 implementation approval and completion |
| Reader locks/restart (L15–L16) | U1 specifies lock scope, limits, recovery permissions and retryable busy behavior; U5 measures large retained datasets and tests restart races. No millisecond guarantee. | U5 completion |
| Reproduction/compatibility (L18/L20/L38) | U1 fixes signatures, identity fields and callable contracts; U3–U5 enforce the command matrix and refusal cases. | Artifact freeze and U5 completion |
| Read-only diagnostics (L21) | U1 clarifies reference §11.5; U4 proves application state/log remain unchanged through diagnostic capture. | U4 completion |
| Client modes and logs (L28/L49) | U1 records exact client versions/modes and a demonstration of discovery, invocation and complete ordered call/result capture; U2 implements a recorder where needed. | U2 freeze |
| Loopback fetch and pin identity (L29/L34) | U1 specifies destination validation, no redirects/proxies, page framing and fresh export ids; U3 tests the actual connection and replacement/restart behavior. | U3 completion |
| Largest export and scratch (L33/L63b) | U1 measures the largest reachable task-queue state and boundary fixtures, recording encoded size, peak pin/scratch/process memory and live-heap effects; U3 repeats against the implementation. | U1 design approval; U3 completion |
| Imported query and privacy (L45 / old plan assumption) | U1 prototypes a query module using an exposed service state type and a private-state query in its defining module. Saved nominal names are already module-qualified; acceptance across compiled programs still needs a prototype. | U5 design/implementation |
| History sufficiency (L67 / old plan assumption) | U1 runs diagnosis traces across a snapshot and decides whether the post-snapshot tail suffices. Record lost information and archive cost if needed. | Retention format freeze |
| Frozen models/settings (L50) | U1 verifies supported immutable identifiers/settings or documents limits of version guarantees, checks availability, and freezes an explicit unavailability policy. No unsupported reproducibility claim. | U2 freeze and U6 comparison |
| Statistics (L51b) | U1 specifies the between-round rule, effect/margin, allocation, uncertainty/power calculation and frozen-baseline ceiling policy. Record executable calculation and assumptions. | U2 freeze |
| Baseline oracle (L52) | U1 designs an out-of-band grader using known fixture inputs and hidden correctness checks, usable before Phase 4 instrumentation. U2 validates it; U5/U6 cross-check the new fix checker against it. | U2 freeze and final grading |
| Publication scan (L55) | U2 implements and tests an exact procedure over the chosen publication artifacts, including known tokens and their transport encodings. Phase 3's recorded search is not an existing reusable scanner. | Publishing any round artifacts |
| Approval provenance (L62) | Preserve the audit's unverified historical provenance. Record U1 approval and subsequent scope decisions with actual artifact/session pointers when they occur. | Claims of approval, not starting authorized U1 work |
| Host/build access (L66) | U1 demonstrates the intended agent can obtain the correct program/build and run loopback fetch or read a stopped owner-only folder. Proxy-only limitations are explicit. | Full workflow evaluation |

## Planning Contract

### KTD1. Bounded, identified exports fetched to private files

Keep the explicit file-fetch command because it bounds and validates delivery independently of how a native client handles large tool replies. Current client behavior varies by version and mode; do not claim every client always places the full result in model context. The MCP reply ceiling is 32 KiB (`MO_LIMIT_MCP_RESPONSE_BYTES`), while an individual saved payload can reach 16 MiB (`MO_LIMIT_SAVED_BYTES`). Account for both text and structured result representation when sizing pages.

A pin owns immutable encoded bytes. Its export id is independent of the log sequence and unique across replacements and service incarnations; sequence is provenance, not page identity. Every response carries the id, artifact kind/version, sequence, index/offset, total length and integrity information fixed by U1. The fetcher enforces the total bound, consistent metadata and exact coverage before accepting a file. U1 selects the identifier/integrity construction and tests overflow/restart behavior; no cryptographic authenticity claim follows from a plain checksum.

Pin creation occurs at an idle safe point. Retained bytes, encoding scratch and page work have named budgets separate from handler memory; existing saved-codec scratch currently uses the live heap and cannot be reused unmodified as proof of isolation. Copy into independently owned bytes, release temporary references before the next handler and verify no retained sharing changes handler allocation/reuse. Include the sum of live memory, pin, scratch and transport overhead in the process sizing result. Resource refusal is explicit. Each pin expires after a named idle bound or is released by completion/replacement; a released pin is refused. Progress is conditional on availability and client activity, not guaranteed under repeated replacement.

Fetch accepts only plain HTTP loopback destinations. Reject userinfo, unsupported schemes and non-loopback addresses before sending a token; resolve and constrain the actual socket peer, including every retry, to loopback. Disable redirects and environment/system proxies. Define strict URL, response/header/body, page-count, aggregate-byte and work bounds. Remote operators run the command on the service host.

Create output privately with mode 0600, refuse an existing destination or symlink by default, and publish only a completely validated artifact. Remove incomplete private temporary files on failure. Do not overwrite an existing permissive file and rely on `umask` to fix its permissions. U3 introduces fuzz coverage for the HTTP/MCP/page readers; U5 adds the offline artifact-reader cases.

### KTD2. Operator dispatch, complete state coverage and client names

Use one overview and focused view tools, with wire names beginning `mo-`, listed after author tools. The prefix is outside Mo identifier syntax, but clients can normalize it: the audited Codex 0.166.0 replaces hyphens with underscores and handles collisions. Test raw names, model-visible names and correct routing in each frozen client mode, including an author `mo_state` alongside operator `mo-state`. Do not require spelling preservation or infer a routing bug from normalization alone.

Dispatch before `mo_service_send` and read only at service idle points through `vm.h` accessors. Operator tools never join the mailbox or supervisor accounting. Use `readOnlyHint`/`idempotentHint` and bounded, useful descriptions. Keep JSON shaping in embedded Mo alongside the existing MCP runtime, following implementation condition 4.

U1 selects and prototypes a complete state representation: either a separate bounded encoder driven by runtime/saved-schema metadata, or a coordinated extension of compiler shape reachability and every verifier/consumer. Existing MCP nominal shapes start from author-tool arguments/results and do not cover every legal service state. U3 must pass AE11 and keep language privacy unchanged. Oversized views return an explicitly truncated summary with an export reference.

### KTD3. Best-effort atomic failure capture

Use a separate diagnostic table, preserving existing table layouts and `user_version`. A failure has two framed records, pre-call state and message/metadata, bound to one id and atomically committed with retention updates. The message record includes tool/timer identity, ordered argument schema/data, arrival time, recovery sequence, structured failure signature, recorded budgets and program/runtime identities. Record schemas distinguish unavailable optional diagnostic details from complete replay inputs.

U1 budgets state bytes, arguments, metadata/framing, encoding scratch, retention and SQLite transaction overhead separately. Existing updating messages can already fill their encoding allowance, while read-only calls do not share that aggregate admission bound. Reserve explicit metadata space or refuse capture with a precise reason; do not silently tighten message admission. A complete record may approach two 16 MiB payloads before framing, scratch and transaction costs, not one 16 MiB write. Lower named caps are allowed only with explicit refusal behavior and evidence.

At controlled-call return, capture bounded inspection data while frames remain. Keep pre-call inputs alive until their serialization finishes, then use normal cleanup; do not retain cancelled frames across subsequent executions. Attempt persistence before supervisor handling and the reply. A successful record id is published only after commit. Preparation/allocation/size refusals and clean rolled-back diagnostic refusals leave the handler failure counted once and expose an unavailable-record reason. A failed rollback, ambiguous commit or unusable connection is not assumed safe: establish its state through the designed store contract or stop under existing persistence-failure policy. Do not retry the diagnostic blindly or count it as a second handler failure.

Attempt capture for the final stopping failure before exit. The in-memory event ring records bounded events without durable records, such as capture refusal/restart/stop; stopped history promises only persisted data. U4 defines deterministic failure-kind grouping and eviction with finite count/byte limits; floods and more distinct kinds than capacity have explicit behavior.

### KTD4. Logical read-only access and restart coordination

Implement a dedicated history/replay store reader. Do not call `mo_service_open`, which can evaluate intervals or snapshot. Distinguish a live owner's lock from a hot journal left by a killed process. Hot-journal rollback may write database pages: the command is logically read-only for application state/logs, not physically incapable of writes. Require owner permissions for recovery; otherwise report recovery required rather than silently bypassing locks or treating an immutable SQLite open as safe.

Hold the lock only while establishing a consistent bounded copy of the necessary persisted data, then close before offline evaluation. Specify maximum bytes/records/work, cleanup on cancellation and a diagnostic for exceeded limits. Measure lock duration on boundary datasets; do not promise milliseconds. A tail of 100 records at up to 16 MiB each alone makes that assumption unjustified.

`mo serve` waits a named bounded time for transient reader contention at startup. U1 defines the timeout result and service-manager retry policy together; a bounded wait alone does not prevent the current non-retried exit-1 failure. U5 tests wait success, timeout, repeated contention, cancellation and recovery. Land any required startup coordination before the reader ships.

### KTD5. Reconstruction, compatibility and fix verification

Factor the existing service controlled-call path for offline execution without starting timers, enqueueing messages or persisting state. Frames survive work cancellation today and are unwound during service cleanup; the new work is bounded inspection/report capture before that cleanup, not a new stack-retention mechanism. Preserve cleanup and subsequent-call behavior.

Store separate identities and validate them before decoding/execution:

| Operation | Required identity/compatibility |
| --- | --- |
| Fetch | Supported envelope/transport version, consistent export generation, lengths and integrity. Record producer provenance; fetching bytes does not by itself require the producer's exact runtime build. |
| Exact replay | Original compiled-program identity and original runtime-build identity, plus valid typed state/message and supported format. Source locations must correspond to that program. A source path or program checksum alone does not identify the runtime build. |
| History | Matching program/schema to interpret values and a supported store/artifact version; v0.1 uses the original runtime build for reconstruction. U1 defines metadata-only refusal output without executing mismatched data. |
| State query | Matching state schema and pure callable contract; added query functions are allowed. v0.1 uses the producer runtime build for evaluation, without requiring the original whole-program checksum. |
| Failure query / revised fix run | The query runtime uses the recorded build; state and ordered argument schemas/count match, time has its specified position/type, and the selected callable/result contract fits its mode. Handler code may differ. |
| Verified fix | Exact replay of the original first, followed by the compatible revision succeeding with recorded limits and the saved-copy check for an updating handler. Persist/report both outcomes and their identities. |

The v0.1 build restrictions are an explicit policy, not an existing loader feature; U1 specifies build provenance beyond a commit label (relevant build configuration included). Cross-build execution compatibility and schema migration are deferred. U1 fixes timer/read-only/updating callable and result shapes rather than treating every state-compatible function as a handler. Ordinary queries may return arbitrary pure results; only the designated handler mode receives a fix verdict.

U1 also specifies how the original build is retained and invoked, and the policy for legacy folders or records produced before offline replay is available. An identity field alone does not provide an executable replay path. Missing provenance or an unavailable/unsupported original runtime must produce an explicit refusal or unverified result; never silently substitute the current runtime and claim exact reproduction.

Use a versioned structured outcome signature. Match defect reason and original source location, or the specific exhausted limit with its recorded budget. Capture stack/variables as available diagnostic context, bounded independently from the complete replay inputs. Do not compare terminal formatting byte-for-byte or accept any generic memory error as a match.

Return explicit statuses for reproduction and verification, including `unverified` when the original did not reproduce. A successful revised run may still be reported as an observation in that case. Heap limits do not restore mailbox occupancy; there is no live-memory-fitness guarantee. U1 defines JSON and exit statuses using the existing line-oriented `mo test --json` conventions; history and view payloads share a schema.

### KTD6. Pilots, frozen baseline, equivalent grading and publication

Extend the existing harness to start isolated service variants and register them with the selected native MCP clients. U1 verifies exact supported client modes and ordered call/result/error capture. U2 adds a local recorder only where transcripts cannot supply complete evidence, and verifies streaming/error paths as well as successful calls. The same registration/recording setup is used in both measured rounds.

U1 fixes an out-of-band baseline grader from known seeded pre-state, arrival time and arguments plus hidden scenario correctness checks. It must run before failure recording/query commands exist. Freeze its source, scoring rules and runtime policy; compile/evaluate each submitted fix under its session's pinned runtime without secretly giving the baseline a later binary. U5 checks the new fix verdict against this independent oracle where their contracts overlap. Mechanical replay success alone need not establish that the agent diagnosed and correctly fixed the application bug.

Within U2, pilots may change scenarios and grading under recorded predictions. Then freeze immutable model identifiers where supported, all observable settings, client versions/modes, prompts, fixtures, budgets, graders and sample allocation before a fresh baseline. Preserve all pilot attempts separately. If configurations cannot be held stable, apply the predeclared limitation or rebaseline policy; never call an alias immutable without evidence.

U1 specifies the primary metric and analysis unit, stratification across scenarios/families, effect/margin, uncertainty and multiplicity treatment, session counts and whether point estimates suffice. Store the calculation inputs and runnable calculation. A frozen baseline at ceiling is recorded honestly; stop the claimed improvement experiment or begin a newly declared experiment with a new independent baseline under the predeclared policy. Never replace only the inconvenient observations.

Before publication, scan the actual selected raw and rendered artifacts, metadata and supported encodings for known evaluation credentials. Test the scanner with seeded leaks; fail on unreadable/unsupported publication inputs. Keep unredacted originals private and preserve grading/reproducibility information in publishable copies. An expired token is still not publishable merely because it is expired. Phase 3's reported search is evidence of that run, not a reusable scanner implementation.

### KTD7. One plan with milestone evidence

This document owns scope, order and acceptance. The design document holds interfaces, budgets, models and experiment results as they become concrete. Record any change to a shared contract here and in its design section before dependent code lands. Unresolved investigations are assigned by the gate table; they are not requests to repeat already settled user decisions.

### Technical Flows

```mermaid
flowchart TB
  A[Operating agent] -->|MCP, one token| E["mo serve /mcp"]
  E --> T[Author tools]
  E --> O["Operator tools: mo- prefix on wire"]
  T -->|mailbox| L[Service loop]
  O -->|idle inspection| L
  L -->|log, snapshot, separate diagnostics| S[(service.sqlite)]
  O -->|identified pages| X[Private export file via fetch]
  A -->|offline query or replay| X
  A -->|stopped history or replay| S
```

```mermaid
sequenceDiagram
  participant F as fetch
  participant S as service
  F->>S: authenticated export request
  S-->>F: page 1, export id A, sequence N, total and integrity
  F->>S: next page of A
  S-->>F: page of A
  Note over F,S: service may commit; A's bytes remain unchanged
  Note over F,S: a replacement B can have the same sequence N
  F->>S: next page of released A
  S-->>F: refused; never substitute a page of B
```

```mermaid
sequenceDiagram
  participant L as service loop
  participant D as diagnostic capture
  participant S as store
  participant V as supervisor
  L->>D: handler failure; frames and pre-call inputs available
  D->>D: bounded report/encoding before cleanup
  alt preparation succeeded
    D->>S: atomic pair insertion and eviction
    S-->>D: committed, clean refusal, or unsafe/uncertain store
  end
  D-->>L: saved id or unavailable reason and store classification
  alt ordinary persistence remains safe
    L->>V: count original handler failure once
    V-->>L: continue, restart or stop
  else unsafe persistence
    L->>L: existing store-failure stop policy
  end
```

## Verification Design

### Proof Boundaries

Lean already proves Core type safety and determinism; it does not currently prove lowering preserves meaning or that the complete C runtime refines the model (`formal/README.md`). Core fuel is evaluation depth, not the VM instruction counter (`formal/TEXT.md`), and reached Simulation operations are outside the current evaluator. Keep these boundaries in all Phase 4 claims.

TLC explores finite instances of the specified transition system. Neither a TLC pass nor an action-to-C mapping is a proof that C implements the model. Name atomicity and fairness assumptions, model intermediate failure states, and connect counterexamples to C regressions. Do not assume atomic publication merely to prove atomic publication; represent the preparation/commit/publication boundary and identify the SQLite behavior relied on.

CBMC checks the C code reached by each harness under its stated assumptions and bounds. Keep unwinding assertions and reachability checks; a proof of safe refusal does not establish successful decoding of every valid input. Differential tests sample agreement between the Lean oracle and C; exclusions for resource cutoffs and unmodeled operations remain visible.

### Required Artifacts and Checks

| Property | Artifact and implementation connection | Owner / completion gate |
| --- | --- | --- |
| Typed reconstruction preserves the selected state and ordered arguments | Extend `formal/` with an executable reconstruction/schema model and meaningful preservation lemmas. Declare any assumed wire-decoder contract, then exercise the corresponding C decoder with typed, malformed and cross-schema cases. Do not present the conditional lemma as a proof of the C reader. | U1 defines theorem statements/assumptions; U5 implements and checks them. |
| A fix verdict requires reproduced original failure plus successful compatible revision | Model the outcome relation and decision procedure in Lean; prove soundness against an independently stated verdict contract, including the unreproduced-original case. Compare representative supported C outcomes to the model. | U1 specifies; U5 delivers proofs and differential cases. |
| Accepted exports never combine generations | Small TLA+ model of create, page, replace, expire, commit and restart; finite TLC configurations with at least same-sequence replacement and restart, plus C page-reader/state-machine regressions. | U1 defines actions/invariants; U3 delivers model/checks. |
| A published failure is complete and diagnostics preserve application recovery state | Separate TLA+ model of preparation, transaction steps, refusal, crash/recovery, publication and eviction; include supervisor counting and unsafe-store stop. Turn relevant traces into store/fault/process tests. | U1 defines assumptions; U4 delivers model/checks. |
| New C readers preserve memory/arithmetic bounds and enforce framing | Extend or add scoped CBMC harnesses for critical new parsing/length/index checks; fuzz all new outside-input paths. State bounds, modeled callees and uncovered properties. | U3 fetch/pages; U4 diagnostic records; U5 offline artifacts. |
| Runtime-specific work/heap/storage behavior meets the contract | C unit tests, allocation/store fault injection, sanitizers and process crash/restart tests. Verify exact work-stop capture and cleanup; keep the live-mailbox/offline mismatch regression. | U3–U5 with their implementations. |

New Lean modules must be included in the actual `lake build` targets and check entrypoints. Preserve the repository's no-`sorry` discipline and inspect theorem axioms; do not discharge a target property by adding it as an axiom or hiding it in a precondition. Pin TLC tooling/configurations and add a reproducible `models` check group to `ci/check.sh` when models land; this is a planned group, not one that already exists. Retain command/version/configuration logs, assumptions, counterexamples and relevant seeds. For progress claims, name fairness and no-expiry/no-replacement conditions.

For each important property, state the prediction first, obtain the Working Agreement's independent check and retain failures. Add negative controls that break the export-generation rule, atomic diagnostic publication and fix-verdict rule and require the intended check to reject them. Tests/proof clients should express required behavior independently of implementation branches; do not merely restate that an evaluator is a deterministic function.

### Safe-c Assessment and Optional Adoption Investigation

Safe-c is a useful source of verification practice. Inspection on 2026-11-03 used upstream commit `eebb98049e2cf0f5e9edffe892ae1f830cbaa1e5`; no safe-c proof suite was rerun for this plan. Its [contract guide](https://github.com/robertguss/safe-c/blob/eebb98049e2cf0f5e9edffe892ae1f830cbaa1e5/docs/guide/contracts.md) demonstrates explicit caller obligations, exact success/failure postconditions, mutation tests and independent property clients. Its [model documentation](https://github.com/robertguss/safe-c/blob/eebb98049e2cf0f5e9edffe892ae1f830cbaa1e5/models/README.md) maps TLA+ actions to C, describes parameterized TLAPS safety proofs, and connects bounded GenMC harnesses to the actual channel/mailbox sources under the C11 memory model. Those are complementary forms of evidence, not an end-to-end proof of arbitrary adopters.

Use the contract, independent-property, negative-control and model-to-C mapping practices in Phase 4. U1 may assess one isolated byte-framing or bounded-buffer component for library reuse or an ACSL/Frama-C proof over existing Mo code. Record the chosen property, pin, reproducible proof result, waivers, caller obligations, integration cost and Mo budget/allocator compatibility. Adopt only if it closes a concrete verification gap without weakening existing guarantees; otherwise retain the techniques and close the investigation with a documented rejection. The investigation is bounded by one component and is not a wholesale migration prerequisite.

Safe-c's [slice contract](https://github.com/robertguss/safe-c/blob/eebb98049e2cf0f5e9edffe892ae1f830cbaa1e5/include/safec/slice.h) assumes valid backing storage and a live owner; lint does not automatically discharge those obligations for all callers. Its library proof coverage and waived guards must be reported precisely. The [historical Mo lint trial](https://github.com/robertguss/safe-c/blob/eebb98049e2cf0f5e9edffe892ae1f830cbaa1e5/docs/evidence/sc18/mo-base-lint.md) documents allocator/API differences on an older revision; later safe-c changes addressed some of its findings, so its counts are not current Mo measurements. Its older plan to pilot Mo Phase 3 is not an active dependency for this Phase 4 plan.

Keep Lean as the main semantic proof environment and existing CBMC as the default C proof tool. Consider Frama-C/WP only for a selected unmet C contract. TLAPS is useful if a protocol needs a proof for arbitrary model sizes beyond TLC's finite cases. GenMC becomes relevant when actual C11 concurrent/atomic algorithms are introduced; the Phase 4 service protocol work does not itself require replacing the scheduler. Rust/Verus or another runtime language remains a separate future decision.

## Implementation Units

Each unit may ship in multiple PRs. File lists identify integration areas; U1 records exact public signatures, formats, limits and working command names in the design document. No downstream unit treats a deferred gate as already proven.

### U1. Design, Feasibility and Verification Contracts

- **Goal:** Settle user-visible behavior and demonstrate the prerequisites for implementing it and collecting comparable evidence.
- **Covers:** R2–R4, R20, R24, R27–R28; KTD1–KTD7; every U1-owned dependency gate.
- **Files:** `docs/design/introspection.md` (new), `learnability/phase4/protocol.md` (new), evidence fixtures/scripts under `learnability/phase4/` or a documented test location, `docs/reference/README.md` (§11.5/§15), `docs/design/architecture.md` (§9), `docs/ROADMAP.md`, `tests/conformance/reference.tsv`; planned formal/model artifact map.
- **Work:**
  1. Define tool and CLI contracts, artifact framing/identity, state-view representation, outcome/callable schemas, capture/retention budgets, transaction classification, history depth and reader/startup coordination.
  2. Run and retain the maximum-state/pin/scratch sizing experiment, imported/private-query prototype, snapshot-history diagnosis traces, and host/build-access demonstration. These are investigations; U1 is not documents-only. Keep prototype tooling out of evaluation agents' environments and pin their baseline binary to a revision without Phase 4 instruments.
  3. Demonstrate supported client modes, name normalization/collision behavior and ordered MCP call/result capture. Verify model configuration availability and specify what happens if an endpoint cannot preserve a version.
  4. Specify the independent baseline oracle and between-round statistics, with runnable sample-size calculation, pilot/freeze rules and missing-session/ceiling policies. Write predictions before pilot or measured sessions.
  5. Specify Lean theorems, protocol actions/invariants, C proof candidates, negative controls and model-to-code test mappings. Record the bounded safe-c assessment if pursued; do not add a dependency merely because it has proofs.
  6. Clarify the read-only diagnostic rule and stale roadmap/reference/architecture descriptions, and obtain Robert's design approval with an evidence pointer.
- **Verification:** Every preflight has a command, versions, inputs and result or a recorded blocker. Imported-query work must preserve normal privacy. The sizing record includes scratch and process totals, not just encoded payload length. Embedded-doc/reference checks pass; experimental C receives the normal relevant build checks.
- **Done:** Design and evaluation protocol approved; predictions recorded; U1-owned preflights closed or explicitly scope-resolved; later unit obligations and evidence locations are concrete.

### U2. Pilots and Independent Baseline

- **Goal:** Establish a usable, frozen evaluation and an independent baseline before Phase 4 instruments exist.
- **Covers:** R20–R24; KTD6.
- **Depends on:** U1 approval and client/model/access/statistics/oracle preflights.
- **Files:** `learnability/harness/`, `learnability/phase4/variants/`, separate pilot records, `learnability/phase4-round-1/` (`freeze.md`, prompts, transcripts, gradings, results, notes), `docs/status/phase4-run.md`, publication scanner/tests.
- **Work:**
  1. Start isolated variants with known fixtures, private credentials and the pinned baseline `mo`. Record native MCP calls/results/errors beside existing CLI logs; validate recorder completeness where needed.
  2. Validate the independent grader before using it. Pilot and harden scenarios, retaining every attempt outside the measured baseline.
  3. Freeze scenarios, models/settings, clients, budgets, graders, analysis and sample counts. Collect fresh baseline sessions. Apply missing-artifact and no-selective-rerun rules.
  4. Report the entire frozen baseline and intervals. If it cannot support the predeclared improvement comparison, apply the stop/new-experiment policy; never quietly harden and overwrite it.
  5. Implement the publication-token scan over the actual artifact formats, with seeded leaks and failure handling; preserve private originals.
- **Tests:** Seeded failures fire before sessions; calls and errors are ordered and complete; missing session artifacts receive the protocol's declared treatment; the publication scan rejects seeded raw/encoded leaks; the baseline grader needs no Phase 4 command.
- **Done:** Pilots and independent baseline are separate; frozen protocol is applied without selection; baseline gate and publication checks pass; U6 configurations remain identified.

### U3. Operator Tools and Export Delivery

- **Goal:** Authenticated views and a bounded state export fetched to a private file.
- **Covers:** R5–R10, R26, R28; KTD1–KTD2; AE1, AE3, AE10, AE11.
- **Depends on:** U2 baseline gate; U1 state-encoding, identity and resource design.
- **Files:** `src/tools/protocol*.c`/headers, `runtime/mcp*.mo` and a views module, `src/vm/vm.h`, `src/vm/service.c`, saved-codec/scratch integration, relevant compiler/bytecode readers if the shape closure changes, CLI command sources, `src/base/limits.h`, `tests/test_protocol_server.c`, `tests/test_service_runtime.c`, `ci/check_operator.py` (new), CLI goldens/guide checks, fetch/page fuzz targets, scoped CBMC harnesses, export TLA+ model and runner, `docs/design/sandbox-properties.md`, guide/design updates.
- **Work:**
  1. Add idle-only accessors and bounded event views, operator catalog/dispatch and complete state encoding. Preserve application scheduling and supervisor accounting.
  2. Implement independent pin/scratch ownership, fresh generation ids, page validation, expiry/replacement and bounded rate control; repeat the sizing experiment against actual code.
  3. Implement loopback-only fetch with redirects/proxies disabled, strict input/output bounds, safe destination handling and complete-file publication.
  4. Deliver export TLA+ checks, C regressions from relevant traces, critical C proof harnesses and fuzzing as part of the reader's introduction.
- **Tests:**
  - Auth refusal; no state/log/count change; full-mailbox inspection; private state with scalar read-only tool and timer.
  - Multi-page state round-trip while commits occur; two pins at the same sequence; stale/reordered/duplicate/missing/mismatched pages; replacement, expiry and restart; malformed/truncated/oversized HTTP/MCP input.
  - Existing destination/symlink refusal; partial-file cleanup; mode 0600; wrong token produces no accepted artifact.
  - Non-loopback resolution/peer refusal, redirect refusal, proxy-environment bypass, and no credential on refused paths.
  - Peak pin/scratch/process accounting and no retained live-heap sharing affecting later handler execution.
  - Both `mo_state` and `mo-state` discover and route correctly through each frozen native client; record wire and model-visible names and catalog size.
- **Done:** Operator driver, goldens, named limits, guide, fuzz/CBMC scope and export model pass. Deliberately breaking generation checks is detected. Failure export uses this path once U4 provides records; U3 does not claim a failure export already exists.

### U4. Failure Capture and Retention

- **Goal:** Complete retained diagnostics where capture succeeds, explicit refusals where it cannot, with safe persistence and unchanged application recovery behavior.
- **Covers:** R11–R13, failure export in R7, R26, R28; KTD3; AE2, AE12.
- **Depends on:** U3 export/view implementation and U1 capture/store classification.
- **Files:** `src/host/store.c`, `src/host/host.h`, `src/base/hostops.def`, `src/vm/service.c`, relevant inspection/process code, `src/heap/saved.c`, limits and reporting, store/runtime/recovery tests, fault-injection build rules, `ci/check_kill_loop.py`, failure-record fuzz target/corpus, capture TLA+ model, relevant CBMC harnesses, saved-state/sandbox/design/guide docs.
- **Work:**
  1. Add the table and atomic record-pair/eviction operation without changing recovery tables or version. Implement and test clean refusal vs unsafe/uncertain store outcomes.
  2. Capture bounded outcome/stack context before normal cleanup, then encode state/message with explicit scratch and metadata limits. Do not change admission limits to conceal a capture failure.
  3. Report saved id or unavailable reason before supervisor/reply handling; preserve one count for the original failure and existing terminal persistence behavior.
  4. Implement deterministic retention, final-stop capture and failure export. Keep diagnostics out of application state/recovery logging, including read-only-call failures.
  5. Deliver the capture model, trace regressions, fuzzing and relevant C proofs.
- **Tests:**
  - Defects, work/memory/size limits, timer failures and read-only failures; application `Error` and admission refusal exclusions.
  - Exact-limit updating arguments plus metadata, large read-only arguments, near-limit pre-state, exhausted heap, allocation/encoding failure and transaction-write refusal.
  - Clean rollback permits later ordinary commits; rollback failure/uncertain commit does not produce a fabricated saved id or unsafe continuation. No double supervisor count.
  - Final stopping failure is attempted before exit; missing record is explicit. Atomic insertion/eviction survives crash boundaries without mixed halves.
  - Timer flood and distinct-kind overflow follow the bounded policy; snapshots leave retained failures replayable; old and new stores preserve recovery compatibility.
  - Frame capture precedes cleanup; cancellation releases resources and allows the expected subsequent service behavior.
- **Done:** Capture/refusal cases, every recovery check, retained-failure export, kill-loop/fault tests, fuzz/proof scope and capture model pass. Diagnostic capture cost is measured for both records plus overhead.

### U5. Replay, Query, Fix Check and History

- **Goal:** Offline commands implement the reconstruction and compatibility contract with explicit reproduction/verification results.
- **Covers:** R14–R19, R26, R28; KTD4–KTD5; AE4–AE7, AE9.
- **Depends on:** U4 and the U1 imported-query, history and lock/startup decisions.
- **Files:** CLI/execute/new command sources, `src/host/store.c`, controlled-call/inspection code in `src/vm/`, query-entry checking/lowering integration, `src/tools/render.c`, codes/errors, limits, artifact-reader fuzz corpus and scoped CBMC harnesses, runtime/CLI/recovery tests and drivers, `formal/` and differential fixtures/checks, guide/reference §13.4/conformance/design docs, service-manager documentation/configuration as needed.
- **Work:**
  1. Implement the dedicated logical read-only reader and hot-journal contract; coordinate reader locks with bounded startup waiting and retryable timeout behavior before shipping the reader.
  2. Enforce separate format, runtime-build, compiled-program and schema checks. Validate state and ordered arguments before executing any query/replay.
  3. Factor the service controlled call and bounded source report; implement structured reproduction matching, arbitrary pure query results and designated handler fix-check mode. Require the original program as well as the revision for a verified fix.
  4. Implement persisted history and named offline limits. Keep normal module privacy; imported queries use exported types/accessors, and private representation queries stay in the defining module.
  5. Deliver Lean reconstruction/verdict proofs, supported differential cases, independent grader cross-checks, and the C resource/crash tests outside the model's scope.
- **Tests:**
  - Every intentionally reproducible seeded defect/work/size case matches its structured signature from folder and export; different reason/location/limit is not reproduced.
  - Bounded work-stop stack/variable reporting, unavailable diagnostic fields, cleanup and later execution.
  - Live-mailbox memory failure with successful unchanged offline handler remains unverified; successful compatible revision cannot bypass original reproduction.
  - Added query function accepted; changed state/arguments/count/order rejected; wrong original program/build refused separately from malformed format; missing legacy provenance and unavailable original runtime follow the documented policy; arbitrary pure result is not misgraded as a handler fix.
  - Imported exposed-state query and defining-module private-state query succeed; capability-bearing entry/capture refused.
  - Matching persisted history without starting timers; live-owner refusal; hot-journal recovery permissions; reader cancellation; startup wait success/timeout/retry under large data.
  - Truncated/edited/oversized artifacts refuse safely; the fuzz target reaches the new path; proof and test negative controls detect relaxed compatibility/verdict checks.
- **Done:** Command drivers/goldens, reference/guide/conformance, relevant Lean and CBMC checks, differential cases, fuzzing and boundary tests pass. Reproduced originals and revision outcomes are both reported; no broader live-memory claim is made.

### U6. Final Evaluation and Release

- **Goal:** Apply the frozen comparison, publish evidence and release only on the declared gate and release decision.
- **Covers:** R22, R24–R28; KTD6; AE8, AE13.
- **Depends on:** U5 and continued availability/declared compatibility of the frozen evaluation configurations.
- **Files:** `learnability/phase4-round-2/`, `docs/status/phase4-run.md`, README, roadmap, guide, reference §15, conformance map and task-queue runbook.
- **Work:**
  1. Reuse the frozen harness, models/settings, client modes, prompts and grader. Expose the guide/catalog as the instructions for new instruments. Apply any unavailability policy before collecting replacement observations.
  2. Record all rates, uncertainty and the predeclared between-round result. Report instrument coverage separately. Preserve failed predictions and incomplete sessions according to protocol.
  3. Run the publication scan on both rounds and pilot artifacts selected for publication. Publish proof/model scope and actual check evidence without turning skips into passes.
  4. On a passing evaluation and Robert's release decision, update promises/install/status, tick Phase 4, document operator credential and stopped-folder rules, and tag v0.1.
- **Done:** The frozen comparison and coverage gates pass, publication checks pass, evidence and limits are documented, release docs agree, and the approved tag exists. A missed prediction leaves the phase open.

## Verification Contract

| Check | When | Evidence and limit |
| --- | --- | --- |
| `bash ci/check.sh gcc` | Before every PR | Build/unit result; include its final line in the PR body. |
| Clang build and relevant unit tests | C changes | Check compiler-specific diagnostics as required by `AGENTS.md`; do not infer this from GCC passing. |
| `bash ci/check.sh drivers static` | Edits to embedded guide/reference/library map | Embedded docs, goldens, guide claims, conformance and static checks. |
| `MO_ALL_DRIVERS=1 bash ci/check.sh sanitize drivers` | U3–U5 C/guide changes requiring sanitizer-backed drivers | Drives the sanitizer executable as well as discovered builds. `sanitize drivers` alone can select debug first and does not establish this evidence. |
| `make deps`, `make tidy`, `make format-check` | Relevant C PRs | Dependencies, checked results and clang-format 14 style. |
| New-reader fuzz smoke, registered in `make fuzz-smoke` / `ci/check.sh fuzz` | U3, U4 and U5 when each reader lands | Fetch HTTP/MCP/pages, diagnostic records and offline artifacts; seeds and bounded runs are empirical evidence. |
| Relevant CBMC harnesses plus reach checks | Critical new C readers/arithmetic and changes to existing proved components | Record input/unwinding bounds, assumptions and properties. Full boundary run follows repository policy. |
| `bash ci/check.sh lean` | PRs changing the formal model or corresponding semantics | Build the proofs and run relevant C-vs-Lean checks before merge; record exclusions. |
| Planned `bash ci/check.sh models` | Export/capture model and corresponding protocol changes | Pinned TLC configurations, invariants/progress assumptions and negative controls; group lands with models. |
| Fault injection and long kill-loop run | U4, relevant U5 reader/recovery changes | Actual allocation/transaction/process behavior; record seeds/commands and keep failures. |
| Frozen grading and statistical calculation | U2 and U6 | Independent baseline/final comparison; Wilson intervals describe individual rates, not by themselves improvement significance. |
| Publication scan | Before any evaluation artifact publication | Known credential-leak checks over the actual publication set, tested with seeded failures. |
| Nightly: `gcc clang sanitize drivers static lean bench`, `CI=1`, `MO_ALL_DRIVERS=1` | Daily at 07:17 UTC and manual dispatch | Existing workflow is not triggered after every merge. Do not substitute an assumed nightly result for required pre-merge evidence. |

When a required tool is missing, report a skip locally and a failure under `CI=1`, following repository policy; neither closes a required evidence gate. Add model check documentation/CI integration when its runner lands. Follow `AGENTS.md` for detached long checks and wait for completion before ending the implementation turn.

## Definition of Done

- **U1:** Approved design/protocol, predictions, completed feasibility/client/access experiments, statistical calculation, identity/callable/transaction contracts and assigned verification artifacts. Remaining uncertainty is explicit and cannot silently pass a gate.
- **U2:** Pilots retained separately, fresh independent baseline frozen and recorded, independent grading validated, complete traffic evidence and publication scan tested.
- **U3:** Views/private-state fixture, loopback fetch and generation isolation, bounded memory accounting, native-client collision checks, new-reader fuzz/proof scope and export model pass.
- **U4:** Complete-or-explicitly-unavailable capture, safe transaction classification, read-only nonmutation, retention and crash recovery, failure export, fuzz/proof scope and capture model pass.
- **U5:** Command and compatibility cases pass, original-first fix verdict enforced, Lean proofs/differential cases delivered, exact VM resource and reader/startup behavior tested.
- **U6:** Frozen evaluation meets its gate, coverage/publication checks pass, claims match evidence, release docs updated and Robert's approved v0.1 tag exists.

Remove abandoned executable experiments before marking implementation PRs ready, while retaining experiment results, counterexamples and failed predictions as evidence. The audit remains a historical review of the original plan; it is not a second implementation checklist.

## Sources and Audit Resolution Map

Repository sources: `docs/reference/README.md`, `docs/design/architecture.md`, `docs/design/implementation.md`, `docs/design/bytecode.md`, `docs/design/services.md`, `docs/design/saved-state.md`, `docs/design/sandbox-properties.md`, `docs/status/phase3-run.md`, `docs/agent-guide.md`, `src/base/limits.h`, `formal/README.md`, `formal/TEXT.md`, `cbmc/README.md`, `learnability/protocol.md` and `AGENTS.md`. Their cited behavior is a design input; this plan does not claim their future Phase 4 extensions already exist.

| Audit finding | Integrated resolution |
| --- | --- |
| PA-1: offline memory/fix overclaim | R15/R19, AE9, KTD5 and U5 require original reproduction and narrow the verdict. |
| PA-2: capture resource/transaction gaps | R11, AE2/AE12, KTD3 and U1/U4 budget preparation and classify persistence safety. |
| PA-3: incomplete MCP state shapes | AE11, KTD2 and U1/U3 require complete state encoding and a private-state fixture. |
| PA-4: client name normalization | KTD2 and U1/U3 test both normalized names and routing; no spelling guarantee. |
| PA-5: check selection/nightly timing | Verification Contract uses `MO_ALL_DRIVERS=1` and the actual daily/manual schedule. |
| PA-6: U3 reader fuzz gap | KTD1 and U3 introduce fetch/page fuzzing with the reader. |
| PA-7: selected-baseline bias | R20–R24, AE13, KTD6 and U2 separate pilots from an independent frozen baseline. |
| PA-8: cancellation-frame premise | KTD3/KTD5 and U4/U5 capture existing frames before cleanup. |
| PA-9: missing formal work | R28, Verification Design and U1/U3/U4/U5 assign concrete Lean, TLA+ and C obligations. |

All remaining audit dependencies are assigned in the gate table. Implementation readiness means U1 can start with concrete outputs and stop conditions, not that its investigations have already succeeded.
