---
title: Repair Phase 5 evaluation capture and collect a fresh cohort
type: fix
date: 2026-11-09
artifact_contract: ce-unified-plan/v1
product_contract_source: ce-plan-bootstrap
execution: code
origin: docs/plans/2026-11-08-0011-feat-phase5-evolves-while-running-plan.md
---

# Repair Phase 5 evaluation capture and collect a fresh cohort

## Goal Capsule

**Objective:** Give maintainers a trustworthy answer to whether both frozen agents can complete the live-upgrade task, with each failure attributed to the behavior that actually failed.

**Means:** Repair evaluation capture, prove the instruments, then run a separately declared four-session experiment (KTD1–KTD4).

**Authority:** The user's follow-up scope and this Product Contract govern this work; the original M6 plan and approved upgrade design continue to govern runtime behavior. This is a separate follow-up, not an amendment to the original results.

**Execution profile:** One follow-up PR. The executor implements, verifies, obtains independent code review, and opens the PR. Do not merge, tag, release or deploy. Start implementation in an isolated checkout from an explicitly identified M6 baseline; preserve other threads' work.

**Stop conditions:** No scored session starts without proven model provenance, complete request attribution, passing instrument controls, and frozen inputs. An unavailable evidence channel blocks collection; it never licenses a model substitution or a relaxed gate. A below-gate result remains published and leaves Phase 5 open.

---

## Product Contract

### Summary

Repair the causes of instrument-related misses and clarify the final-answer instruction. Collect four fresh sessions only after development controls show that the declared clients can produce the required evidence. Four passes are the desired result, not a promised outcome or a reason to keep rerunning.

### Problem Frame

Experiment 2 officially scored **0/4**. All four sessions performed the upgrade and passed the corrected state/restart diagnostics, but shell-origin requests remained unmatched. Codex slots also lacked actual-model evidence; Claude slots returned explanatory final messages instead of only the revision. The original grader mishandled qualified migration names; that correction already exists and must be retained.

The official result, corrected diagnostic analysis and runtime correctness are different facts. Missing traffic attestation must not be mislabeled as a proven persistent-identity mismatch.

### Key Decisions

- **Separate follow-up plan** — session-settled: user-directed; the user chose a new plan rather than editing the M6 plan. Governs R1, R8.
- **A fresh, honest measurement** — session-settled: user-approved scope; fix capture and instructions, preserve the old score, and collect four fresh sessions rather than tune the original evidence. Governs R1, R2, R6.

- **Preserve direct shell usage** — session-settled: user-directed; transparent capture was selected over a new launcher requirement. Governs R3.

### Requirements

- **R1. Preserve history.** Keep experiment 1, experiment 2's official 0/4, original streams and qualified-grader analysis unchanged. New controls and results have distinct experiment identities.
- **R2. Preserve the pass gate.** Declare exactly four fresh slots, two per frozen client, in Codex/Claude/Codex/Claude order, with the existing 3/4 gate requiring at least one pass per client. Every launched slot counts, including timeouts, client errors and missing evidence; no replacement sessions or best-of selection.
- **R3. Account for every request.** Preserve ordinary direct curl, Python urllib and `mo push` through the supplied proxy, without a new launcher or agent-side correlation requirement. Reconcile native MCP and shell HTTP requests transparently with collector-owned evidence, including request arguments, response/completion and origin. Unknown, ambiguous and unanswered traffic remains explicit and fails required attestation.
- **R4. Prove actual models.** Retain Codex 0.166.0 requesting `gpt-6.1-sol` with `xhigh`, and Claude Code 2.1.317 requesting `claude-opus-5-5`. Establish trusted session-bound actual-model evidence before collection; configuration and agent-authored text are insufficient. Retain fallback events and apply the predeclared missing/substituted-model miss rule.
- **R5. Preserve task correctness.** Keep the existing migration, job/payload/attempt/state/lease/next-id, revision, source-test, committed ordinal and folder-only restart checks. Support qualified migration selectors using the existing correction, independent recompilation and attested activation.
- **R6. Make the answer contract unambiguous.** After observing the upgraded live revision, the agent's entire final message must be solely that observed value. Grade the raw final answer with the existing whitespace trimming; never extract a number from prose or repair the answer afterward.
- **R7. Preserve collection integrity.** Keep both credential boundaries, protected fixtures, backend denial, proxy recording, source capture, timeout cleanup and publication scanning. Development controls stay outside the denominator. Freeze the protocol, supplied artifacts, clients, binaries, graders and instruments before the first scored launch.
- **R8. Report the outcome without closing unrelated gaps.** Publish every new session and miss, an overall score and separate diagnostic dimensions. Carry forward the pause-margin miss and all existing residuals; no phase-closure or release claim follows automatically from this evaluation.

### Scope Boundaries

In scope: Phase 5 evaluation scripts, their controls, a narrowly scoped shell capture instrument, the task prompt, a new experiment declaration/freeze, collected artifacts, and status documentation.

Out of scope: Mo runtime changes, production MCP tools, an active-bundle read tool, new TLS support, authorization changes, a general agent framework, retrospective rescoring, or altered task difficulty. Keep service-token-only access insufficient for upgrades, ordinary views read-only, and `mo push`'s loopback and credential-read rules intact.

**Deferred to follow-up work:** production pause optimization and release approval. Preserve limits concerning compiler-child isolation, native work accounting, initialization stalls, scratch cleanup, sustained rate starvation, parsing duplication and E1428 reachability. Do not add an E1428 requirement here.

---

## Planning Contract

### Key Technical Decisions

**KTD1. Qualify model provenance before collecting.** Add typed adapters for trusted client/provider metadata and bind records to the exact session and model-serving turns. Separate requested model, actual primary model, auxiliary events and fallback. The existing recursive search for any `model` key is not sufficient. U1 must establish the supported source with the exact pinned clients; if none exists, publish the blocker and stop collection. Changing client/model pins or accepting weaker provenance requires a user decision and a new declaration.

**KTD2. Capture ordinary commands transparently (per R3).** Extend collector-side observation to correlate proxy request occurrences with the actual client or shell process and its descendants. Preserve raw request/result data and native event matching. Do not infer origin by interpreting arbitrary shell/Python source, matching a push-looking string, trusting an agent-supplied correlation header, or relying on timing alone. Prove a supported process/connection observation mechanism in development before freezing it; if reliable attribution cannot be established without new agent obligations, report the readiness blocker and do not collect. Keep the real `mo push` executable and ordinary direct curl/Python usage. Capture records stay collector-owned and cannot be forged by session processes.

**KTD3. Separate diagnostics while keeping the conjunctive pass rule.** Report task correctness, independently derived activation identity, command-origin push proof, traffic completeness, model provenance, final-answer compliance and publication integrity separately. Missing proof is a distinct failure, not inferred state corruption. Overall success still requires every declared dimension; accepted reports alone do not grant credit.

**KTD4. Freeze one new experiment after controls and independent review.** Preserve the existing fixture and supplied source/context artifacts. Disclose the transparent recording mechanism without adding invocation requirements; clarify the exact whole-message answer rule. Hash the complete inputs and declare all four slots before collection. Do not revise an instrument, prompt, gate or model pin during a cohort. If an instrument defect appears, retain launched outcomes, stop and document unlaunched slots; a subsequent experiment requires its own declaration.

### High-Level Technical Design

These sketches describe responsibilities and evidence flow, not implementation APIs.

Component relationships:

```text
Native client events ----------> native occurrence matcher ----+
Direct curl/Python -> recorded HTTP + process evidence --------+-> session grader
Real mo push -> recorded HTTP + process evidence --------------+
Trusted model metadata -> typed adapter -----------------------+
Private source/state/restart checks ---------------------------+
```

Protocol sequence:

```text
Qualify clients -> prove collector controls -> independent review
-> freeze declaration and inputs -> reserve slot -> launch client
-> stop/capture -> hidden checks -> redact/scan -> publish all outcomes
```

Lifecycle and branch decisions:

```text
Preparation -> qualification
  unavailable provenance/origin -> blocked; no scored launches
  proven controls -> frozen -> collecting
    input drift before launch -> stop; record reason
    session miss -> retain slot; continue unchanged cohort
    instrument defect -> retain launched slots; stop cohort
    four terminal slots -> publish score -> gate met / phase remains open
```

Data flow and retention:

```text
Private raw events + secrets -> protected immutable evidence
-> redacted publication copy -> credential scanner + completeness check
-> published session bundle + dimension results + experiment summary
```

### Grounding and Dependencies

Baseline research used M6 commit `4dffd7e2776a3ff7a9a7245c7131b8ef330308d5` and PR #318. At execution, identify its accepted successor explicitly. The merged nightly repair was `814ddd0f70e5fd613130fe87a9cd24cd187af8e6`; verify ancestry and recorded verification rather than including another thread's unmerged repair.

Read `learnability/phase5/protocol.md`, `evaluation/{harness,proxy,grade}.py`, their tests, and `results/experiment-2-original/slot-{1,2,3,4}/stream.jsonl`. Phase 4's proxy and publication scanner remain reusable instruments; avoid changing Phase 4 semantics to fix Phase 5. `grade.migration_selector` and `harness.grade_collected` already implement the qualified-selector correction.

The pinned Codex stream's actual-model channel is **unproven**. Current official [non-interactive documentation](https://learn.chatgpt.com/docs/non-interactive-mode) is a starting point for U1, not proof that a feature exists in frozen 0.166.0. This uncertainty blocks collection, not the bounded instrument work.

---

## Implementation Units

### U1. Establish model-evidence readiness

**Goal:** Know whether the frozen clients can satisfy R4 before spending scored slots.

**Requirements:** R4, R7; KTD1.

**Files:** `learnability/phase5/evaluation/harness.py`, `test_harness.py`; a new development-only model-provenance note and controls under `learnability/phase5/evaluation/`.

**Approach:** Inspect supported metadata sources for the exact installed versions. Implement typed extraction with explicit session/turn association. Run a bounded development characterization with each pinned client, retaining raw evidence and failures. Declare the characterization count before launch; do not probe repeatedly until a preferred outcome appears. If actual serving-model evidence is unavailable, record the precise missing capability and halt scored collection without substituting clients.

**Test scenarios:** Valid trusted metadata passes; requested configuration alone fails; `model` in tool output or assistant text fails; wrong-session, missing, conflicting or truncated records fail. A fallback is retained and scored under R4. Real pinned-client characterization proves availability beyond synthetic fixtures.

**Verification:** Focused harness tests and retained development evidence identify the trusted event schema, exact client hash/version and coverage across all serving turns. Unsupported capability yields a documented collection blocker.

### U2. Transparently record shell and push origins

**Goal:** Make real shell inspection observable without accepting unrelated requests.

**Requirements:** R3, R7; KTD2. Can proceed independently of U1; collection waits for both.

**Files:** `learnability/phase5/evaluation/proxy.py`, `harness.py`, `test_proxy.py`, `test_harness.py`; new narrowly scoped collector-side observation adapter and its tests in the same directory.

**Approach:** Extend existing proxy request sequence/result records with trusted invocation correlation. Prove the process/connection observation mechanism before relying on it. Support native MCP, ordinary unwrapped curl/Python inspection and real `mo push`, without new agent invocation rules. Preserve raw and normalized tool names. Deduplicate started/completed client events by event identity and match duplicate requests by occurrence. Keep correlation records outside the agent-writable area; sanitize publication copies. Remove the broad inference that any push string explains every unmatched upload.

**Execution note:** Write negative controls first, then prove them against the real local service and collector.

**Test scenarios:** Native-only, shell-only and mixed sessions reconcile exactly once. Ordinary unwrapped curl, dynamically constructed urllib and `mo push` through the supplied proxy are positive controls. Reused RPC IDs, duplicate arguments and concurrent invocations remain distinct. Echoing a push, unrelated HTTP beside a real push, forged/replayed correlation, a failed command and unattributable traffic cannot acquire attestation. Normal exit and timeout close the observation lifetime; reparented descendants and late traffic cannot be credited to a completed invocation or another slot.

**Verification:** Real-service controls demonstrate request/process ownership, both credential forwarding/redaction, and continued backend/private-folder denial. Preserve failed controls and their corrections.

### U3. Make grading and instructions precise

**Goal:** Distinguish behavioral failures from evidence failures and remove answer-instruction ambiguity.

**Requirements:** R5, R6, R7; KTD3. Depends on U1/U2 interfaces, without bypassing their stop conditions.

**Files:** `learnability/phase5/evaluation/{prompt.txt,harness.py,grade.py,test_harness.py,test_grade.py}`, existing real-service controls, and `learnability/phase5/evidence.py` with its tests.

**Approach:** Consolidate the whole-message rule in the canonical prompt and remove contradictory appended wording. Extract each client's actual final assistant message using its event schema. Keep the existing qualified migration correction and independent identity derivation. Emit separate diagnostic dimensions and aggregate their declared conjunction. Transparent capture only observes and records; hidden state expectations and reference solutions stay private.

**Test scenarios:** Bare observed revision with allowed whitespace passes; prose, Markdown, wrong revision, intermediate-only text and empty final answers fail. A bare `1` without required live observation does not pass. Bare and qualified migration functions both pass valid reference controls. Existing unchanged/source-only/initializer-substitution/dropped-job/altered-payload/duplicate-completion/wrong-revision controls remain negative. Correct state with missing attestation reports those facts separately and fails overall; wrong identity or extra activation fails even with complete traffic.

**Verification:** Run focused tests, real-service migration controls, and the protected stub collector's success and timeout paths. Verify immutable source restoration and folder-only restart. Run bounded development collector controls with both frozen clients outside the scored denominator, then independently review the instruments before freeze.

### U4. Freeze, collect and publish the new four-slot cohort

**Goal:** Deliver a reproducible result for the unchanged upgrade task.

**Requirements:** R1, R2, R7, R8; KTD4. Depends on successful U1–U3 readiness and instrument review.

**Files:** New experiment declaration, freeze and results under `learnability/phase5/`; `learnability/phase5/evaluation/publish.py` and its publication controls; `docs/status/phase5-run.md`; release-facing notes only where the new evidence changes their factual statements.

**Approach:** Allocate a new experiment ID without overwriting old artifacts. Freeze the exact scenario, 3600-second per-slot limit, four slots, clients/models, supplied source/context, transparent recording configuration, grading rules, build inputs and publication scanner. Compare production build inputs against M6; invalidate reuse claims if they differ. Verify merged-nightly ancestry and actual baseline checks before collection. Reserve slots durably before launching; preserve every miss. Publish dimension results, aggregate score, raw/normalized tool-name records, actual model/fallback behavior and sanitized streams.

**Test scenarios:** Missing readiness proof or changed frozen inputs refuses launch. Concurrent/duplicate slot launch is rejected. A launched timeout or provider error remains in the denominator. Unlaunched slots after a cohort stop are explicitly marked and cannot be reported as completed. Publication rejects either credential, including encoded forms, and detects omitted slots/artifacts. A 2/4 outcome or a 3/4 outcome with no pass from one client fails the gate; 3/4 with both clients represented meets it; 4/4 is reported only if observed.

**Verification:** Run the frozen declaration's exact commands. Independently regrade all dimensions using retained protected inputs, including private state/restart folders and collector receipts. Separately reproduce public score aggregation from the sanitized archive; do not claim public regrading of dimensions whose inputs remain private. Publish the new redacted receipt/model evidence needed for the public audit, and verify the public aggregation path after archive extraction. Record every command's result and any skips. Compare old result hashes before/after publication and report the original 0/4 alongside the new score.

---

## Verification Contract

During implementation, run `PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s learnability/phase5/evaluation -v` and `PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s learnability/phase5 -p 'test_evidence.py' -v`. Retain real-service, protected collector and model-characterization evidence separately from scored sessions.

Before the PR, run `bash ci/check.sh gcc` and record its actual final line. Run `bash ci/check.sh drivers static` if embedded guide/reference/library-map files change. C changes are outside this plan; if a required defect demands one, stop to identify the scope change and apply the original both-compiler, relevant sanitizer/fuzz obligations before any new freeze. Wait for every check and process to finish; record skips as skips.

Run `git diff --check`, the existing publication scanner with both credentials and encoded forms, and an archive round-trip/completeness check. Obtain independent `ce-code-review` before opening the PR. Retain corrections and failed assumptions; remove abandoned executable code without deleting evidence.

A model/attribution readiness failure is a documented blocked collection, not a completed evaluation. No checks, clients or collection are run merely to write this plan.

## Definition of Done

- U1 proves actual-model evidence for both frozen clients, and U2/U3 controls prove attribution and grading before the cohort is frozen.
- U4 publishes exactly four terminal scored slots with every miss, or explicitly reports blocked/incomplete collection without claiming this objective achieved.
- The original official 0/4 and qualified analysis remain intact; new dimensions are independently regraded from protected inputs, while public aggregate scores reproduce from the sanitized archive.
- Repository-required checks and independent review finish; the follow-up PR describes actual evidence and limits.
- No abandoned implementation remains; no processes, temporary access rules or collectors remain running.
- A below-gate cohort leaves the phase open. The existing pause-margin miss and Robert's explicit release approval remain separate requirements even after a passing cohort.
