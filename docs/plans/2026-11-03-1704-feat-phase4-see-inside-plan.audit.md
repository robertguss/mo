# Audit: Phase 4, Agents Can See Inside It - Plan

> Follow-up, 2026-11-03: the user requested consolidation, and the [implementation plan](2026-11-03-1704-feat-phase4-see-inside-plan.md) now incorporates PA-1 through PA-9 and the discussion decisions. This report preserves the audit of the original plan (SHA-256 `d190032b5e7f401bd1b4e6847b61d132ec3ee304b030e75f112e227dcf7c5f43`). References below to the unchanged plan and its line numbers describe that audited version. This note is not an independent re-audit of the revision or evidence that its implementation gates have passed.

- **audit_of:** `docs/plans/2026-11-03-1704-feat-phase4-see-inside-plan.md`
- **date:** 2026-11-03
- **auditor:** T3 Code / Codex harness, gpt-6-astra, xhigh reasoning
- **mode:** interactive
- **capabilities:** subagents yes, web yes, file writes yes, shell yes
- **draft independence:** compromised: the parent read mixed goal/summary/constraint sections containing design choices before extracting the brief. The fresh drafting agent saw only the cleaned excerpt and allowed repository files. The combined label follows the skill's stricter rule.
- **target repo:** `/home/exedev/code/mo` at `0b15ebc4eb93bf17afae409391a91c8f1b4b7517`; tracked files clean, with one pre-existing untracked file: `docs/plans/2026-11-02-0039-feat-task-queue-service-plan.md`
- **notes:** Markdown input inside the target checkout. No earlier audits read. The plan is an overview with later milestone designs; deliberately deferred names, formats and measurements are not treated as missing implementations.

## Verdict

**Revise** (20 of 121 ledger entries unverifiable)

The architecture holds up: authenticated live inspection, typed offline exports, separate diagnostic retention, and replay through the service execution path fit the repository. Revise the memory/fix-check guarantee, failure-capture resource design, private-state encoding coverage, client-name premise, and verification steps before treating the overview as an implementation contract. These changes preserve the approach; the evidence does not warrant Rethink. The remaining unknowns mostly belong to the explicitly planned design step, not a claim that the proposed features already work.

### Top issues

1. PA-1: An offline fix can pass under the recorded heap ceiling while still failing in the original live heap.
2. PA-2: Two bounded records do not establish that capture can succeed at the payload or memory boundary.
3. PA-3: The existing MCP representation does not cover every legal private service state.
4. PA-4: The installed Codex client rewrites the proposed operator names.
5. PA-5–PA-8: Correct validation selection, cover the fetch reader when introduced, account for baseline selection, and reuse cancellation frames that already survive.
6. PA-9: Assign Phase 4 explicit Lean work and scoped model checking, following the user's added formal-verification requirement.

## Findings

### PA-1. The fix checker can certify an unchanged memory-failing handler

- **Severity:** High
- **Category:** consistency
- **Evidence:** L3–L5b. R15 explicitly admits that memory failures may not reproduce without the mailbox (`plan:85`), but R19 and KTD5 promise that a passing fix fits the budget the live run had (`plan:89,247`). Queued arguments occupy the same heap as the handler: [service.c:599](../../src/vm/service.c), [limits.h:315](../../src/base/limits.h).
- **Suggested fix:** Apply the agreed v0.1 contract: first replay the original handler against the saved failure, then check the revised handler under the reconstructed offline conditions. If the original failure does not reproduce, a successful revised-handler run must leave the fix unverified for that failure. Even when the original reproduces and the revision passes, the result establishes success for this reconstructed case, not fitness under the original live mailbox's memory pressure or every future workload. Require a regression where the *unchanged* original handler fails with a populated mailbox and succeeds without it; it must not be labeled a verified fix. Narrow the unconditional “and nothing else” claim in the problem frame.
- **Discussion resolution:** The user agreed to this narrower guarantee after an explanation of omitted mailbox memory. Reconstructing the original live memory pressure is not required for v0.1. This settles the intended revision; PA-1 remains a finding against the unchanged plan until that contract is incorporated there.

The instruction budget is reproducible through the existing controlled call. A heap ceiling is different from the available space below that ceiling; using the same number does not reconstruct its occupants.

### PA-2. Failure capture has no complete resource-boundary contract

- **Severity:** High
- **Category:** missing-work
- **Evidence:** L9b/L22 and L79. The existing updating-message envelope has a 25-byte header and may already consume the complete saved-payload allowance ([service.c:38](../../src/vm/service.c), [:598](../../src/vm/service.c)). Adding failure kind and budgets needs accounted space. Read-only admission bypasses that aggregate saved-message bound. Encoding the state also allocates schema scratch from the live heap ([saved.c:138](../../src/heap/saved.c)); exhausted space can refuse it before any store write ([heap.c:441](../../src/heap/heap.c)). The risk table estimates a 16 MiB write, although KTD3 allows two separately bounded records.
- **Suggested fix:** In U1, budget the state, argument encoding, diagnostic metadata, framing, scratch and transaction overhead separately. Define metadata headroom or an explicit capture-size refusal without silently changing existing message admission. Bound capture allocations and expose failures that happen *before* SQLite, as well as store-write refusal. Under the agreed policy, a failed capture reports that the record is unavailable and does not itself stop the service while ordinary persistence remains safe. If the underlying store cannot safely persist updates, retain the existing store-failure stop policy. Add boundary/fault tests for exact-limit arguments, read-only failure arguments, exhausted heap, the final stopping failure, clean diagnostic refusal and an unusable shared store. Correct the maximum write accounting.
- **Discussion resolution:** The user agreed to best-effort diagnostic capture with explicit missing-record reporting, including failures during preparation before a write. Service continuation is conditional on ordinary persistence remaining safe. This settles the behavior policy for PA-2/L11; transaction classification, resource accounting and tests remain design/implementation work, and the plan remains unchanged.

Splitting state from message is useful, but is not itself a proof that every failure is representable or that its representation can be allocated. This is particularly important because memory exhaustion is one of the events the recorder promises to preserve.

### PA-3. A valid private state can fall outside the existing MCP encoder

- **Severity:** High
- **Category:** missing-work
- **Evidence:** L36/L45. MCP shape reachability starts only at tool arguments and results, not service state or timers ([service-reach.c:111](../../src/bytecode/service-reach.c)). Nominal conversion asserts that the shape exists ([protocol-value.c:28](../../src/tools/protocol-value.c)), and the verifier rejects extra unreachable shapes ([validate.c:723](../../src/bytecode/validate.c)). Private state is legal ([reference:1112](../../docs/reference/README.md)). U3's task-queue-only driver can miss this because an updating tool result already reaches the queue state.
- **Suggested fix:** Add an explicit U1/U3 decision for state-view encoding: extend shape reachability and all compiler/verifier readers, or use a separate state encoder based on the complete runtime/saved schema. Add a service fixture with private nominal state, a scalar read-only tool and a timer; its state view and export must succeed. Retain normal language privacy for query source: place queries in the defining module or use exposed accessors.

The saved codec already records nominal names, cases and fields ([saved.c:95](../../src/heap/saved.c)), and those nominal names already include the defining module ([types.c:180](../../src/lower/types.c)). Reuse that evidence rather than leaving qualification wholly unverified until U5. It does not imply that the MCP bridge has the same coverage.

### PA-4. Native Codex rewrites the supposedly unchanged operator names

- **Severity:** Medium
- **Category:** false-claim
- **Evidence:** L30b. KTD2 says `mo-` names survive the clients unchanged and cannot be rewritten into author-tool names (`plan:244`). The installed client reports `codex-cli 0.166.0`. Its versioned [sanitizer](https://github.com/openai/codex/blob/rust-v0.166.0/codex-rs/codex-mcp/src/mcp/mod.rs#L567) replaces hyphens with underscores; its [normalization caller](https://github.com/openai/codex/blob/rust-v0.166.0/codex-rs/codex-mcp/src/tools.rs#L131) applies it and hashes collisions.
- **Suggested fix:** Keep `mo-` as a server-side namespace if desired, but remove the unchanged-client-name guarantee. Freeze supported clients and test an author tool named `mo_state` alongside an operator named `mo-state`, including discovery and invoking both. Document the distinction between wire and model-visible names. Correct the `mo.*` spelling in the architecture diagram.

This is evidence of normalization, **not** of incorrect routing: the examined client retains raw names and handles collisions. Calling one operator against today's queue does not test the disputed property.

### PA-5. The verification commands do not establish two of their advertised checks

- **Severity:** Medium
- **Category:** false-claim
- **Evidence:** L60b/L60d. The sanitizer/driver row promises a fresh sanitizer guide check (`plan:480`), but [check.sh:142](../../ci/check.sh) selects a debug executable first unless `MO_ALL_DRIVERS=1`. The “After each merge” nightly row (`plan:485`) contradicts [nightly.yml:3](../../.github/workflows/nightly.yml): daily 07:17 UTC and manual dispatch only.
- **Suggested fix:** Specify `MO_ALL_DRIVERS=1 bash ci/check.sh sanitize drivers` after the relevant builds, or explicitly drive the fresh sanitizer executable. Change the nightly timing to daily/manual; if per-merge evidence is required, explicitly require a manual dispatch and its result. Do not imply the existing workflow automatically supplies it.

### PA-6. U3 introduces a reader before its fuzz obligation is assigned

- **Severity:** Medium
- **Category:** missing-work
- **Evidence:** L42. U3 introduces the fetch client's HTTP/MCP response and page consumption (`plan:378,385`), while the fuzz verification row covers only U4/U5 (`plan:482`). The repository requires fuzz coverage for every outside-input reader ([implementation.md:24](../../docs/design/implementation.md)).
- **Suggested fix:** Assign fetch-response/page-reader fuzz coverage to U3, with malformed/truncated responses, page identities, aggregate limits and released-pin cases. Reuse an existing target where it demonstrably reaches the new parsing path; keep the separate export-file reader target in U5.

### PA-7. Hardening on observed baseline success can manufacture apparent improvement

- **Severity:** Medium
- **Category:** risk
- **Evidence:** L53b; `plan:19,94,156,352`. R21 conditions accepted scenarios/runs on their observed baseline rate. Even with one hardening attempt, conditioning on sufficiently low baseline observations can make an independent final round look better without any change in true success probability. A pure binomial calculation in Run notes confirms that mechanism. The exact comparison rule remains L51b, unverifiable until U1; this finding is rated below High because U1 can account for it.
- **Suggested fix:** Apply the agreed evaluation design: use pilot sessions to choose and harden scenarios, then freeze the scenarios, grading rules and agent settings before running a fresh independent baseline. Compare the final round against that baseline. Retain all pilot results, including failures, but exclude them from the improvement calculation. Keep this work within the existing baseline milestone. Define the between-round rule before selecting session counts; `wilson.py --size` provides a single-proportion interval-width calculation, not by itself a two-sample comparison or power guarantee.
- **Discussion resolution:** The user accepted the additional evaluation cost of pilot sessions followed by an independent baseline. This settles the selection-policy change; U1 still specifies the statistical comparison, session counts and handling of a frozen baseline that leaves insufficient room. The audited plan remains unchanged.

This does not dispute making scenarios useful or retaining the three promises. It concerns what the resulting evidence can support about improvement beyond chance.

### PA-8. Work-budget reporting should capture existing frames, not add a stack-retention mechanism

- **Severity:** Medium
- **Category:** false-claim
- **Evidence:** L7/L8. KTD5 and the risk table say cancellation drops the VM stack (`plan:247,325`). [execute.c:93](../../src/vm/execute.c) sets cancellation flags and returns with frames intact; [service.c:721](../../src/vm/service.c) unwinds them later. The missing piece is a populated defect/inspection result at cancellation, not surviving frames at the controlled-call boundary.
- **Suggested fix:** Rewrite U1/U5 around bounded inspection/report capture between controlled-call return and service cleanup, reusing existing defect-frame/variable traversal. Preserve the ordinary terminal/cancelled-process contract. Test the exact work-limit frame and variables, then cleanup and a subsequent service call.

### PA-9. Give Phase 4 explicit formal-verification work

- **Severity:** Medium
- **Category:** missing-work
- **Evidence:** The user explicitly requested continued Lean proofs and TLA+ where useful during discussion. L88–L94 confirm the existing foundations and their limits: Core type safety and determinism are proved in [Theorems.lean:25](../../formal/MoCore/Theorems.lean) and [:170](../../formal/MoCore/Theorems.lean); service Simulation operations stop at an unmodeled boundary ([TEXT.md:35](../../formal/TEXT.md)); Core fuel is a depth bound, not the service VM's instruction budget ([:747](../../formal/TEXT.md)). The overview names nightly Lean checks but assigns no Phase 4-specific proofs or executable formal model. This finding incorporates the user's added requirement; it does not claim those new proofs already exist.
- **Suggested fix:** Add a formal-verification section to U1 with explicit properties, assumptions, proof/model artifacts, implementation links and checks for U3–U5. Keep Lean as the primary semantic proof environment. Reuse existing determinism and type-safety theorems, and extend the executable model with meaningful properties of the new reconstruction and verification rules, rather than merely restating that a function is deterministic. Candidate obligations are: accepted artifact restoration preserves the selected typed state/argument tuple, and a reported fix verification requires a reproduced original failure plus a successful compatible revision in the modeled offline environment. Where a decoder or runtime operation is assumed rather than modeled, state that hypothesis and connect it to C tests; do not present conditional semantic proofs as proofs of the C reader.

For the runtime ordering rules, a small TLA+ model is useful. Model export creation, page reads, replacement/expiry, service commits and restart; separately model failure preparation, transaction commit/refusal, crash recovery and eviction. Check that an accepted export never combines generations, a published failure never combines mismatched state/message halves, diagnostic operations preserve application state/recovery position, and capture failures do not double-count supervisor failures. Model mid-operation failures and the relevant atomicity boundaries explicitly, rather than assuming the property being checked. Any progress claim must name its fairness and no-replacement/no-expiry assumptions.

TLC checks finite instances of a specification; it is not a universal proof of the C implementation ([TLC documentation](https://docs.tlapl.us/using:tlc:start), [model-checking scope](https://nightly.tlapl.us/doc/model/model.html)). Pin the tool version and finite configurations, retain counterexamples, and translate relevant traces into implementation regressions. Require meaningful negative controls so the checks detect broken generation/transaction rules. Connect Lean's semantic model to representative supported handler/query cases through differential tests, explicitly retaining the existing exclusions for resource cutoffs and unmodeled operations. Exact VM work stops, heap exhaustion, SQLite behavior and crash durability still need their C, fault-injection and process tests unless separately modeled and linked.

Assign the model/specification work to U1, export invariants and their tests to U3, capture/recovery invariants to U4, and reconstruction/fix-verification proofs plus differential cases to U5. For PRs that change the model or its corresponding semantics, require the relevant Lean checks before merge rather than relying solely on the next nightly. Add a reproducible TLC check to the normal check entrypoint when the TLA+ artifacts land; missing tooling is a skip or failure according to the existing CI policy, never passing evidence. Continue relevant existing CBMC checks for C properties. Record predictions before claiming new guarantees, obtain the Working Agreement's independent check, and retain failures. These are proposed implementation obligations, not work run by this audit.

## Pre-mortem

- A memory-exhausting handler is “fixed” without changing code because the offline run has no queued arguments. The operator trusts a pass that does not reproduce the live constraints (PA-1).
- The most useful failure occurs near a resource limit. Its capture runs out of scratch or its message record has no metadata headroom; a later snapshot removes the state needed to reconstruct it (PA-2).
- The task queue passes the operator tests, but a legal service with private state reachable only through a timer reaches an MCP shape assertion (PA-3).
- Scenario hardening retains unusually low baseline observations. Fresh final trials regress toward their unchanged underlying success rate, and the report attributes that gain to the tools (PA-7).

The strongest case against the *product* is that today's instruments may already solve the practical diagnoses. The plan directly tests that with a baseline and an explicit ceiling stop condition. That is a legitimate gate, not evidence for discarding the architecture now.

## Open questions

The structured questions were sent during verification. Subsequent discussion settled several policy choices, recorded in the entries below and under User-confirmed decisions in Assumptions. Settled policy does not require another approval; remaining implementation, documentation and evidence work is still open. User-asserted answers remain labeled as such rather than treated as implementation evidence. No external evidence pointers have yet been supplied.

| Entries | Question and evidence that would settle it |
|---|---|
| L11 | Policy settled by the user: report unavailable diagnostic records and continue only while ordinary persistence remains safe; an unsafe store follows the existing stop policy. Which transaction outcomes establish that distinction, including failed rollback/uncertain commits? A transaction contract plus fault-test design would settle the implementation premise. |
| L15, L16b | What bounds reader lock holding and what happens if the startup wait expires? Supply measurements/design; otherwise remove “milliseconds” and state the actual bounded-work and refusal contract. |
| L18 | Reproduction criteria settled by the user: a handler defect must match the original failure reason and source location in the original program; a limit failure must reach the same specific limit under the recorded budget. Another generic memory failure is insufficient. Stack frames and available variables support diagnosis; byte-for-byte rendered-report equality is not required. U1/U4/U5 must define and retain the necessary outcome signature and test the comparison. |
| L20 | Compatibility policy settled by the user: a fix check may use changed handler code while requiring compatible state and argument types, including argument count and order. Added state-query functions are allowed when the exported state type matches. Schema changes are refused with an explanation and left to migration support. U1/U5 must define and test the callable/schema checks. |
| L21 | Interpretation settled by the user: read-only calls do not change saved service state or enter the recovery log; failed calls may create separate diagnostic records containing their inputs and pre-call state. U1 must clarify reference §11.5 accordingly and reconcile R11/R27. No further product decision is needed; the current reference remains unchanged. |
| L28, L49 | Which exact native clients/modes are supported, and do their transcripts preserve ordered calls? Versioned demonstrations or the planned recorder fallback settle this. |
| L29b | Are redirects/proxies disabled and actual localhost connections constrained to loopback? Specify socket-level destination policy, not only URL spelling. |
| L33, L63b | What owns pin bytes/scratch, and what does the maximum-state measurement show? U1's sizing result and ownership design settle these; the existing stop condition is appropriate. |
| L34 | Which pin generation/id prevents two exports at the same commit from being stitched? Put it in the page contract. |
| L38 | Replay identity settled by the user: require the original compiled program and runtime build. Fix checks and state queries use the separate compatibility rules in L20. U1/U3/U5 must encode and validate those identities separately; fetch/history details and treatment of build compatibility outside exact replay still require their command-specific rules and tests. |
| L50 | Which immutable model identifiers/settings can be used in both rounds, and what is the policy if one becomes unavailable? Record exact supported configurations before freezing. |
| L51b | Pilot/baseline separation is settled: the user accepted pilot selection and hardening, a freeze, and a fresh independent baseline, with pilot data excluded from the improvement calculation. What comparison rule, expected rates, margin and session counts make the remaining statistical claim supportable? This remains the explicit U1 protocol task. |
| L52 | What frozen oracle grades baseline fixes before the failure recorder/query command exist? Specify parity with final grading without silently changing the baseline binary. |
| L55c | What exact publication-token check covers the chosen artifacts? Phase 3 records a search outcome, not an executable scan to reuse. Define the procedure in U2. |
| L62 | Where are the originating user approvals recorded? A session/decision artifact would verify the plan's provenance; the plan's own labels alone do not. |
| L66 | Is matching source/build and host/SSH access an established operating constraint? Deployment/access evidence would settle it; proxy-only operation is explicitly a reduced feature set. |
| L67 | Is post-snapshot history enough, or is an archive needed? U1's concrete diagnosis scenarios and retention decision should answer this. |

## Assumptions

- **Plan, unresolved:** Shared-store failure recoverability (L11); reader/restart timing (L15/L16b); capture and implementation of the settled reproduction criteria (L18); implementation of the settled callable compatibility rules (L20); documentation of the settled read-only diagnostic interpretation (L21); native-client behavior (L28/L49); actual loopback delivery (L29b); pin ownership/identity/fit (L33/L34/L63b); identity fields, validation and remaining command-specific compatibility rules (L38); frozen models and comparable grading/statistics (L50/L51b/L52); token scanning (L55c); approval provenance (L62); host access (L66); and history sufficiency (L67).
- **Plan, supported by source:** Handler purity (L1), single-owner persistence (L13), separate bearer storage (L40), typed saved schemas (L19/L45), and finite task-queue inputs for sizing (L63a). These support the architecture but do not establish future implementation success.
- **Unstated assumptions exposed by the audit:** Recording a heap ceiling reconstructs live memory conditions (L5b, refuted); splitting two payloads makes capture feasible at all boundaries (L9b/L22, refuted); existing MCP type metadata covers arbitrary state (L36, refuted); and selected baseline observations need no selection accounting (L53b, refuted).
- **User-confirmed design decision:** The user agreed that v0.1 must reproduce the original failure before a passing revision can verify a fix for that reconstructed case; a successful offline run does not establish fitness under omitted live mailbox memory pressure. This resolves PA-1's proposed contract change, not its implementation or the other open questions. Statements of earlier user approval remain plan-reported, not independently confirmed.
- **User-confirmed capture policy:** Preparation and write failures explicitly report an unavailable diagnostic record. The service continues if ordinary persistence remains safe; an underlying store that cannot safely persist updates follows the existing stop policy. This is the agreed behavior for PA-2/L11, not evidence that the transaction implementation can yet distinguish those cases.
- **User-confirmed read-only interpretation (L21, user-asserted):** Read-only calls never change saved service state or enter the recovery log. A failed read-only call may produce a separate diagnostic record of its inputs and pre-call state. The user agreed this is what the reference should express. The policy is settled; reference wording and implementation verification remain pending.
- **User-confirmed reproduction criteria (L18, user-asserted):** Handler defects match by original failure reason and source location in the original program. Limit failures match by the specific limit reached under the recorded budget, not merely by a broad memory-error category. Stack frames and available variables are diagnostic evidence, and the complete rendered report need not match byte-for-byte. Artifact fields, bounded capture and comparison tests remain to be designed and implemented.
- **User-confirmed compatibility boundary (L20/L38, user-asserted):** Replay requires the original compiled program and runtime build. Fix checks accept changed handler code with compatible state and argument types, including argument count and order. State queries accept added functions while requiring the exported state type to match. Changed state structure or argument schemas are refused with an explanation; converting old data to a new schema is left to migration support. This settles the v0.1 boundary, not the implementation of its checks or unstated build rules for other commands.
- **User-confirmed evaluation design (PA-7, user-asserted):** Use pilots for scenario selection/hardening, freeze scenarios/grading/agent settings, then collect an independent baseline for the final-round comparison. Retain pilot evidence but exclude it from the improvement calculation. The user accepted the extra sessions within the existing baseline milestone; exact sample sizing and statistical rules remain U1 work.
- **User-added formal-verification requirement:** Keep actively using Lean to prove relevant properties, and use TLA+ if useful. PA-9 proposes the scope, concrete obligations, implementation connections and checks. This instruction is not evidence that any new theorem or model has already been checked.
- **Blind-draft assumptions:** The cleaned excerpt accurately summarized the desired outcome; export should support local analysis without a new language; exact replay concerns handler execution rather than arbitrary OS/network/storage failure; operator artifact access is intended but requires an explicit confidentiality policy. These assumptions shaped the independent draft, not the plan's authority.

## Run notes

- The plan's original and final SHA-256 is `d190032b5e7f401bd1b4e6847b61d132ec3ee304b030e75f112e227dcf7c5f43`. Only this audit report was added. The pre-existing untracked task-queue plan was not modified.
- Setup used read-only Git status/HEAD/root inspection and a byte hash. Source inspection used `rg`, numbered file excerpts and a Python path-existence inventory. Missing files explicitly designated as new work were not treated as errors.
- A fresh native subagent wrote the blind draft, then verified runtime/store claims. Two other native subagents checked protocol/client and evaluation/verification slices. The parent owned the ledger, checked consequential findings, and narrowed literal or speculative objections. No subagent read prior audits.
- No compiler build, repository test suite, service process, SQLite database open, deployment, PR or release action ran. The service/store tests can write databases or use sockets and were unnecessary for this source-level audit. Therefore this report does **not** say the current build or planned features passed runtime tests.
- Installed-client version queries returned `codex-cli 0.166.0` and `2.1.317 (Claude Code)`. Public primary sources were fetched; no authenticated service or client session was started. Codex behavior was checked against its exact version tag. Claude documentation is rolling and does not prove the exact installed version's behavior.
- The pure arithmetic check below made no files and printed both assertions as `True`. Git status was identical before/after. It is a constructed selection counterexample, not a Phase 4 measurement:

```python
from math import comb
from subprocess import check_output
before = check_output(["git", "status", "--short"], text=True)
n = 10
accepted = sum(comb(n, k) for k in range(5))
selected_rate = sum((k / n) * comb(n, k) for k in range(5)) / accepted
print("Selected baseline mean is below unchanged true rate:",
      selected_rate < 0.5)
print("git status unchanged before and after:",
      before == check_output(["git", "status", "--short"], text=True))
```

- External evidence: versioned Codex [name sanitizer](https://github.com/openai/codex/blob/rust-v0.166.0/codex-rs/codex-mcp/src/mcp/mod.rs#L567), [name normalization/collision handling](https://github.com/openai/codex/blob/rust-v0.166.0/codex-rs/codex-mcp/src/tools.rs#L98), and [result handling](https://github.com/openai/codex/blob/rust-v0.166.0/codex-rs/core/src/tools/context.rs#L184); current [Claude MCP output behavior](https://code.claude.com/docs/en/mcp#mcp-output-limits-and-warnings). API character alphabets alone do not establish native-client transformations.
- Smaller corrections: move U3's failure-export permission test to U4 (L69); correct `mo.*` in the diagram (L87); add new conformance mappings in `rules.tsv` as well as `reference.tsv` (L59b); distinguish current inline catalog assertions from future `tests/cli/` goldens (L41b); define the token publication scan instead of citing a historical observation (L55b); distinguish the harness wrapper from the no-web protocol rule (L48c).
- KTD4 was evaluated as *logically* read-only history, with the explicitly mentioned journal rollback. A writable recovery phase can implement that. Literal `SQLITE_OPEN_READONLY` cannot; this qualification is not a claim that the approach is impossible. Its “milliseconds” lock estimate remains unsupported. Likewise, exact schema matching and ordinary input validation are compatible; R18 should not be interpreted to require accepting malformed files or incompatible function arguments.
- The phase-level document may legitimately defer exact files and APIs. Missing names in an illustrative file list alone were not promoted to findings. The new fetch transport fits existing module boundaries (L43). The optional longer history must preserve old reader semantics, for example using a separate archive (L23b).
- Parent extraction read headings and mixed sections at plan lines 13–25, 33–43 and 154–186. Those exposed design choices, so the overall draft is labeled compromised despite the fresh child's isolation. No claim of sealed independence is made.
- During follow-up discussion, formal source/documentation inspection confirmed Lean 4.34.0 in `formal/lean-toolchain`, existing type-safety/determinism proofs and CI differential checks, the unmodeled Simulation boundary, and the distinction between Core fuel and VM work. A repository filename search for TLA+ sources/configurations outside build/vendor paths found none. Official TLC documentation was consulted to assess its scope. No Lean build, TLC run, tool installation or new formal artifact was performed; PA-9 is an implementation-plan recommendation. The follow-up adds seven verified ledger entries and one finding for the user's new requirement.

## Claim ledger

`plan:N` refers to the audited plan; other paths are relative to the target repository. `reference:N` abbreviates `docs/reference/README.md:N`. Confirmed future consistency means the requirement has assigned work and sound premises, **not** that the future feature has run. Some entries are split with letter suffixes to keep one proposition per verdict. Inferred premises are labeled as assumptions or qualified in the evidence.

| ID | Section | Kind | Claim | Verdict | Evidence or reason |
|---|---|---|---|---|---|
| L1 | Problem frame:43 | codebase | Handlers need no recorded external effects. | confirmed | src/vm/service.c:115,173; reference:1105 excludes capabilities/functions. |
| L2 | Problem frame:43 | codebase | Only successful state replacements enter the recovery log; failed calls are discarded. | confirmed | src/vm/service.c:191,695,718; application Error/read-only results do not replace state. |
| L3 | Problem frame:43; R15 | consistency | State, arguments and time alone determine every runtime outcome. | refuted | R15 (:85) itself excludes mailbox-sensitive memory failures; src/base/limits.h:315. |
| L4 | R15 | codebase | Queued arguments consume the handler process heap. | confirmed | src/vm/service.c:599,617; src/base/limits.h:315. |
| L5a | R19/KTD5 | codebase | The existing controlled call supplies a reusable instruction budget. | confirmed | src/vm/service.c:142; src/vm/vm.h:74. Preserve the extra pre-entry control poll. |
| L5b | R19/KTD5 | consistency | Passing the offline failure query proves the fix fits the original live memory conditions. | refuted | Plan:89,247 versus :85; occupied mailbox memory is absent. User agreed to PA-1's narrower offline guarantee; the audited plan remains unchanged. |
| L6a | KTD5 | codebase | Existing generic saved-copy status exposes the specific cause needed by failure classification. | refuted | src/vm/service.c:211,695 conflates encoding/decoding failures; src/heap/saved.c:418 discards detailed cause. KTD5 therefore requires explicit cause plumbing, not merely reusing MEMORY. |
| L6b | KTD5/U5 | codebase | Deterministic saved-value limits provide a basis for reproducing a classified size failure. | confirmed | src/heap/saved.c:12,95,156. Earlier allocation failure must not mask the cause. |
| L7 | KTD5; risk:325 | codebase | Cancellation itself drops the VM stack. | refuted | src/vm/execute.c:93 returns with frames intact; src/vm/service.c:721 later unwinds. PA-8. |
| L8 | KTD3/U4 | codebase | State and message lifetimes allow capture before reply and supervision. | confirmed | src/vm/service.c:684,713,718,721,722,724. |
| L9a | KTD3 | codebase | Each separately framed payload up to 16 MiB fits the existing row ceiling. | confirmed | src/host/store.c:180; src/base/limits.h:340. |
| L9b | KTD3; U4:419 | assumption | Two records alone ensure every near-limit admitted failure fits. | refuted | src/vm/service.c:38,598 permits a full existing message payload; new metadata needs space. Read-only admission bypasses that cap at :601. PA-2. |
| L10 | KTD3/U4 | codebase | An additional table can preserve existing user_version and old recovery behavior. | confirmed | src/host/store.c:86,136 checks only named old tables; :238 reads version/head/snapshot/log. Preserve their layouts and semantics. |
| L11 | KTD3 | assumption | Every diagnostic-write refusal leaves the shared connection safely usable for later commits. | unverifiable | No new transaction implementation. src/host/store.c:344 ignores rollback failure; SQLite pager errors can persist (vendor/sqlite/sqlite3.c:60943,65764). User agreed continuation must depend on safe ordinary persistence; classify clean refusal versus unrecovered store failure. |
| L12 | Scope/history | codebase | Snapshots prune committed-message history. | confirmed | src/host/store.c:332 deletes log; src/base/limits.h:333 sets 100-message interval. |
| L13 | Problem frame/KTD4 | codebase | The running service owns an exclusively locked store. | confirmed | src/host/store.c:133; docs/design/services.md:767. |
| L14a | KTD4/U5 | external | SQLite hot-journal recovery requires writable access despite logically read-only history. | confirmed | Vendored SQLite 3.54.1 refuses read-only rollback (vendor/sqlite/sqlite3.c:64308). KTD4 is feasible as recovery followed by a read without new application data. |
| L14b | KTD4/U5 | external | Hot-journal recovery can preserve application rows without adding tables or records. | confirmed | vendor/sqlite/sqlite3.c:64314,64365 restores existing pages under an exclusive lock. Requires writable recovery; clarify permissions and lock lifecycle. |
| L15 | KTD4 | runtime | The proposed reader holds its lock only for milliseconds. | unverifiable | No measurement; a bounded tail can contain 100 records up to 16 MiB each (src/host/store.c:267; src/base/limits.h:330,333), plus failures and recovery I/O. |
| L16a | KTD4 | codebase | The documented systemd service does not retry exit 1. | confirmed | docs/ops/task-queue.md:85 specifies RestartPreventExitStatus=1 2 69. |
| L16b | KTD4 | assumption | A bounded startup busy wait prevents reader-caused restart failures. | unverifiable | No reader/wait bounds or expiration policy. src/host/store.c:144 and src/tools/serve.c:163,189 currently turn open refusal into exit 1. |
| L17 | U5 patterns | codebase | Existing recovery has reusable identity, typed-message and controlled-call checks. | confirmed | src/vm/service.c:249,259,274,292,298. mo_service_open itself evaluates intervals and may snapshot (:462), so cannot be the history reader. |
| L18 | R15/AE4/KTD3 | assumption | Recorded failure kind is enough to decide the promised outcome/span/variable equality. | unverifiable | User-asserted criteria now require matching defect reason/source location or the specific limit under the recorded budget; report text need not match byte-for-byte. Plan:245's artifact fields remain undesigned, so sufficiency is not established. src/vm/defect.c:24 and vm.h:144 expose the relevant diagnostic distinctions. |
| L19a | Assumption:308 | codebase | Saved schemas include module-qualified nominal names. | confirmed | src/lower/types.c:180; src/codegen/compile.c:3133; src/heap/saved.c:107. |
| L19b | U5:450 | codebase | Importing the original defining type can preserve schema identity. | confirmed | src/lower/types.c:181 uses the defining module, not the importing query module. Planned integration fixture still needed. |
| L20 | R16/R18/U5 | assumption | The failure-query callable and recorded arguments have a defined compatibility contract. | unverifiable | User-asserted policy now permits changed code with compatible state and argument types/count/order and refuses schema changes. src/vm/service.c:274 and src/heap/saved.c:442 provide validation foundations; command-specific checks and tests remain to be implemented. |
| L21 | R11/R27 | assumption | Saving failed read-only calls is consistent with the reference's no-saving rule. | unverifiable | User-asserted intended interpretation: the prohibition covers saved service state and the recovery log, while separate failure diagnostics are allowed. Policy settled in discussion; docs/reference/README.md:1127 still needs clarification in U1. |
| L22 | R11/KTD3 | assumption | Direct reuse of the saved codec always captures state when the live heap is exhausted. | refuted | src/heap/saved.c:138 allocates schema scratch in the live heap; src/heap/heap.c:441 caps it by remaining space. PA-2. |
| L23a | R11/U4 | codebase | An independent failure table survives later snapshots. | confirmed | src/host/store.c:337 deletes only log. |
| L23b | History-depth option:220 | codebase | Old readers require the log to contain only records after the snapshot. | confirmed | src/host/store.c:274. The optional longer-history design must use a separate archive or revisit compatibility; the plan does not commit to retaining old rows in log. |
| L24 | R11/Key decisions | codebase | Supervisor failures exclude application Error and admission refusals. | confirmed | src/vm/service.c:198,603,723. Post-return saved-copy failures also count; persistence failures stop separately (:699). |
| L25 | R17/KTD3 | consistency | Offline history excludes the volatile restart/stop/refused-write ring. | confirmed | Plan:245 explicitly narrows history; U5:446 matches records plus committed log. |
| L26a | KTD1 | codebase | The MCP response ceiling is 32 KiB. | confirmed | src/base/limits.h:473; src/tools/protocol-call.c:71. |
| L26b | KTD1 | codebase | Tool result values appear in text and structured content. | confirmed | runtime/mcp.mo:132. This does not mean every JSON-RPC response is duplicated. |
| L27 | KTD1 | codebase | Saved-value encodings have a 16 MiB ceiling. | confirmed | src/base/limits.h:330; src/heap/saved.c:400,445. |
| L28 | KTD1 | assumption | All intended native-client modes require this file-fetch delivery because they always put large replies into context. | unverifiable | Client modes are not frozen. Codex direct outputs enter model input, but Code Mode differs; current Claude docs describe automatic file delivery for large text. See external sources in Run notes. Explicit fetch remains a reasonable design choice. |
| L29a | KTD1 | codebase | Mo currently lacks an outgoing HTTP/TLS client. | confirmed | rg search of src/host and src/tools for http, tls, client, connect( found listener/server code, no outgoing client. |
| L29b | KTD1 | assumption | The proposed URL checks ensure the actual token destination is loopback. | unverifiable | Future client must define localhost resolution, redirects and proxy behavior. Existing listener numeric-loopback logic is at src/host/listener.c:140. |
| L30a | KTD2 | codebase | A mo- prefixed wire name cannot be an author-declared Mo identifier. | confirmed | docs/reference/README.md:68. |
| L30b | KTD2 | external | The mo- name survives the native clients unchanged. | refuted | Installed Codex 0.166.0 replaces hyphens with underscores; versioned sanitizer and caller cited in PA-4. Collision hashing means no misrouting has been demonstrated. |
| L31 | KTD2/U3 | codebase | The protocol has a pre-mailbox dispatch point and embedded-Mo JSON shaping. | confirmed | runtime/mcp-rpc.mo:111; src/tools/protocol-call.c:116,192; runtime/mcp.mo:15. |
| L32 | U3:391 | codebase | Idle service access remains available with a full mailbox and after a returned cancelled step. | confirmed | src/vm/service.c:532,721; src/tools/protocol-server.c:523,544. |
| L33 | KTD1 | assumption | Pin ownership and encoding scratch will leave handler budgets unaffected. | unverifiable | The new ownership path is not designed in detail. Existing codec scratch uses the source heap and increases its peak (src/heap/saved.c:138; src/heap/heap.c:468). Release scratch and avoid retaining live values. |
| L34 | KTD1/diagram | assumption | The page identity distinguishes pins replaced at the same commit sequence. | unverifiable | Plan:280–289 depicts sequence N only. A distinct export generation/id satisfies the intended rejection promise; page format is deferred. |
| L35 | KTD1 | consistency | A stable pin and separate rate bucket remove commit-induced export restarts. | confirmed | Plan:243 retains bytes across commits. Completion is still conditional on no replacement, expiry or transport failure, all acknowledged there. |
| L36 | R6/KTD2/U3 | codebase | Existing MCP shapes cover every legal saved service-state type. | refuted | src/bytecode/service-reach.c:111 roots only at tool arguments/results; protocol-value.c:28 asserts nominal shape presence. PA-3. |
| L37a | R16/AE7 | codebase | Query purity validation needs entry-specific integration, since ordinary functions may accept capabilities. | confirmed | reference:995; src/check/services.c:208,290; src/check/captures.c:276. This is a premise of U5's planned checker refusal, not a refutation of that future feature. |
| L37b | R16/U5 | consistency | A query-entry purity check can meet the planned refusal without changing language rules. | confirmed | Reuse recursive checks in src/check/captures.c:276 via suitable checker integration; do not weaken ordinary functions. |
| L38 | R18; U3:399 | assumption | Build provenance, exact replay identity and schema-only query compatibility have distinct settled refusal rules. | unverifiable | User-asserted policy requires original program/runtime build for replay and L20's type compatibility for fix/state queries. src/bytecode/image.c:55 provides a program checksum, not runtime-build identity. Separate fields, remaining fetch/history rules and acceptance tests are still needed. |
| L39 | U3/U4 ordering | codebase | U3 can obtain log views without U5's reader; failure exports can follow in U4. | confirmed | src/host/host.h:131 streams records from the existing handle; plan:385 explicitly defers failure contents. |
| L40 | R9 | codebase | Credential storage is separate from service state and messages. | confirmed | src/host/listener.c:75,93; src/tools/protocol-request.c:95 wipes bearer header; src/vm/service.c:21 has no token. R9's author-data exception is respected. |
| L41a | U3 patterns | codebase | Existing catalog paging is reusable. | confirmed | runtime/mcp.mo:63; runtime/mcp-rpc.mo:138; ci/check_cli.py:1887. |
| L41b | Impact:315 | codebase | Existing serve/catalog goldens live in tests/cli. | refuted | rg --files tests/cli found no such goldens; current assertions are ci/check_cli.py:1747 and tests/test_protocol_server.c:205. New golden files are valid planned additions. |
| L42 | U3/Verification | consistency | Every newly introduced reader receives fuzz coverage in its introducing unit. | refuted | U3 creates HTTP/MCP page consumers, while plan:482 schedules new-reader fuzzing only U4/U5. docs/design/implementation.md:24 requires reader coverage. PA-6. |
| L43 | KTD1/U3 | codebase | A native fetch client can fit current module boundaries. | confirmed | docs/design/architecture.md:84; ci/deps.sh:28. New socket transport can live in host and orchestration in tools, with Mo parsing. |
| L44 | KTD1 | codebase | The private-file creation umask pattern exists. | confirmed | src/tools/serve.c:188. This covers creation, not permissions of an existing output file; U3 must define replacement/refusal. |
| L45 | R18/U5 | codebase | Existing nominal/schema metadata supports cross-program query matching without new language rules. | confirmed | src/lower/types.c:179; src/heap/saved.c:107,456; src/check/check.h:629. Private representation remains governed by reference:941. |
| L46a | Sources/Goal | codebase | README states the three operating promises and pre-release status. | confirmed | README.md:3,7,21,65. |
| L46b | Sources/Goal | codebase | Phase 4 is the roadmap's current phase and v0.1 target. | confirmed | docs/ROADMAP.md:12,18,22. |
| L47a | Problem frame:41 | codebase | Phase 3's close used automated recovery and kill-loop evidence. | confirmed | docs/status/phase3-run.md:3,127,185. |
| L47b | Problem frame:41 | codebase | No fresh-agent service diagnosis is recorded in the searched repository evidence. | confirmed | Diagnosis/service regex search excluding plans/audits found none; phase3-run.md:43 records catalog/stats use only. Not a claim about unrecorded sessions. |
| L48a | KTD6 | codebase | The existing harness records mo invocations. | confirmed | learnability/harness/README.md:24; session.py:236. |
| L48b | KTD6 | codebase | The learnability protocol disallows web access. | confirmed | learnability/protocol.md:39. |
| L48c | KTD6 | codebase | The harness itself enforces no network. | refuted | learnability/harness/README.md:17 says it does not start agents; session.py:236–282 wraps mo and prints setup instructions, without network sandboxing. |
| L49 | Assumption:310 | assumption | Intended clients expose complete ordered MCP transcripts. | unverifiable | Historical successful calls do not establish transcript completeness. U1 explicitly validates this and supplies a recorder fallback. |
| L50 | R22/KTD6 | assumption | Identical immutable model versions/settings will be available for both rounds. | unverifiable | No endpoints/settings frozen yet; previous tools-final/freeze.md:31 used a default model, which is insufficient evidence of immutability. |
| L51a | R20/U1 | codebase | wilson.py --size computes a single-proportion interval-width sample size. | confirmed | learnability/wilson.py:64,132. |
| L51b | R20 | assumption | The chosen session count can distinguish the predicted between-round gain from chance. | unverifiable | User agreed to separate pilots from a frozen independent baseline. U1 still must select comparison rule/rates/widths and session counts; --size alone is not a two-sample power calculation. |
| L52 | KTD6/U2/U6 | assumption | Baseline and final have equivalent fixed-build grading oracles. | unverifiable | U2 precedes U4 artifacts and U5 fix command; learnability/protocol.md:21,70 requires the session's binary. An independent baseline oracle is feasible but unspecified. |
| L53a | R21/Stop conditions | consistency | Hardening is bounded to one revision and rerun before stopping. | confirmed | Plan:19,94. |
| L53b | R20/R21/R22 | assumption | The hardening acceptance rule alone leaves an unbiased baseline for the final comparison. | refuted | Selection is conditioned on observed baseline success. User accepted PA-7's pilot/freeze/independent-baseline revision; the original plan remains unchanged. The constructed counterexample is not a Phase 4 empirical result. |
| L54 | R24/KTD6/U6 | consistency | Instrument-use coverage is separate from the diagnosis/fix pass rate. | confirmed | Plan:97,248,352,467. |
| L55a | Token checks | codebase | Phase 3 recorded a successful token-search outcome. | confirmed | docs/status/phase3-run.md:45. |
| L55b | U2:373/U6:469 | codebase | The cited Phase 3 record supplies a reusable publication-token check. | refuted | It gives an observation, not a procedure; content search across ci/learnability/docs/services found no corresponding scanner. Define one in U2. |
| L55c | U2/U6 | assumption | The future token scan covers actual Phase 4 publication formats. | unverifiable | Neither scanner nor transcript formats are fixed; define literal/encoded coverage and private retention of the disposable token until final scanning. |
| L56 | U2:371 | codebase | Missing session files are failures and are not rerun. | confirmed | learnability/protocol.md:57; only pre-task infrastructure failures qualify for rerun. |
| L57 | R27/U1 | codebase | Reference §15 still lists Phase 3/4 as later and says they change no rules. | confirmed | docs/reference/README.md:1708; roadmap:18 confirms Phase 3 is complete. |
| L58 | R27/Documentation | consistency | New command documentation can coexist with no source-language rule changes. | confirmed | Plan:54 explicitly distinguishes tools from language; reference §13.4 already documents commands. Reconcile the separate read-only persistence question L21. |
| L59a | R26/Documentation | codebase | Guide/conformance checks are extendable to the new claims. | confirmed | ci/check_cli.py:1662,3810; ci/check_conformance.py:98,149. New checks still need implementation. |
| L59b | Documentation:334 | codebase | New reference blocks require inventory and evidence mappings. | confirmed | ci/check_conformance.py:140 requires tests/conformance/rules.tsv as well as reference.tsv. Include both in the later exact file list. |
| L60a | Verification contract | codebase | gcc/drivers/static/sanitize group purposes match their stated roles. | confirmed | ci/check.sh:161,208,464. |
| L60b | Verification:480 | codebase | Running sanitize plus drivers ensures guide tests use the sanitizer executable. | refuted | ci/check.sh:142 chooses debug first unless MO_ALL_DRIVERS=1. PA-5. |
| L60c | Verification:485 | codebase | Nightly uses strict missing-tool behavior and all built drivers for the listed groups. | confirmed |  .github/workflows/nightly.yml:24,27,57. |
| L60d | Verification:485 | codebase | Nightly runs after each merge. | refuted |  .github/workflows/nightly.yml:3 has only daily schedule/manual dispatch. PA-5. |
| L61a | U4 kill-loop premise | codebase | The existing kill loop exercises normal business calls rather than supervisor defects. | confirmed | ci/check_task_queue.py:17 launches correct queue; ci/check_kill_loop.py:214 invokes application fail. U4 must add defect-producing fixtures, as its extension anticipates. |
| L61b | U4 | consistency | The plan assigns extension of the kill loop to exercise new failure writes. | confirmed | Plan:407,422,483. Add a seeded supervisor failure and a marker proving the kill happened during its diagnostic transaction. |
| L62 | Key decisions/KTD7 | assumption | The reported user approvals have independent provenance in this checkout. | unverifiable | Plan labels decisions approved but links no originating session; tracked-name search finds only this phase4 plan. No authorization inferred from plan text. |
| L63a | Task-queue dependency | codebase | Task-queue input limits permit constructing a worst-case sizing fixture. | confirmed | services/task-queue/main.mo:9,70 bounds 5000 jobs and 2048-byte payloads. |
| L63b | Assumption:309 | runtime | The largest pinned export fits the limit the design will choose. | unverifiable | No measurement or limit chosen; plan correctly makes sizing a U1 stop condition. |
| L63c | U3 description standard | codebase | Task-queue descriptions define shape, retry behavior and data visibility. | confirmed | services/task-queue/main.mo:55,78,168; ci/check_task_queue.py:136 checks descriptions. |
| L64 | R1–R4/Milestones | consistency | The six implementation units preserve the required stage order and predecessor gates. | confirmed | Plan:62–65,107–114,342,493–498. |
| L65 | R2/R3/Documentation | consistency | Milestone plans refine one shared design rather than replace it. | confirmed | Plan:63–64,191,333,342; names/formats/limits remain delegated. |
| L66 | Actors/Dependencies:204,209 | assumption | Operating agents can obtain the matching program/build and reach the service host for fetch/files. | unverifiable | No current deployment or agent-access evidence; proxy-only agents are explicitly limited to views. Asked whether host/SSH access is an established constraint. |
| L67 | Dependencies:206 | assumption | Post-snapshot history is sufficient for v0.1 operations. | unverifiable | No operator trial yet; U1 explicitly decides retention. Snapshot pruning is confirmed in L12. |
| L68 | R5–R10/U3 | consistency | Each operator requirement has assigned implementation work. | confirmed | R5/R8/R10 -> U3 dispatch/auth; R6 -> accessors/views/ring; R7 state -> pin/fetch, failure -> U4; R9 -> credential separation. Coverage does not prove future tests pass. |
| L69 | R7/U3 test:394 | consistency | U3's stated completion tests are all runnable before U4. | refuted | Plan:385 defers failure exports until U4, but :394 tests a fetched failure export in U3. Move that half of the permissions check to U4; a small stage-order correction. |
| L70 | R11–R13/U4 | consistency | Failure capture, refused-write visibility, independent retention and timer-fair eviction have assigned work. | confirmed | Plan:409–424; counts/bytes/failure-kind definition still belong in U4. PA-2 adds pre-store capture refusal handling. |
| L71 | R14–R19/U5 | consistency | Replay, query, history and their malformed/identity/budget refusals have assigned command/test work. | confirmed | Plan:432–452; PA-1 and L18/L20/L38 qualify guarantees, rather than missing the whole feature. |
| L72 | R20–R24/U1/U2/U6 | consistency | The evaluation has protocol, baseline, final, artifact retention and miss-handling work. | confirmed | Plan:352,365–374,461–469; PA-7 identifies missing handling of baseline selection. |
| L73 | R25–R27/U6 | consistency | Release status, docs, roadmap and tag are gated on the final result. | confirmed | Plan:101–103,462–470,498. No release is authorized by this audit. |
| L74 | AE1–AE3 | consistency | Auth refusal, application Error exclusion and operator non-mutation have acceptance tests. | confirmed | Plan:389–390,415; corresponding current boundaries are L24,L31,L40. |
| L75 | AE4–AE7/AE9 | consistency | Replay, schema acceptance, stopped history, purity and fix-check examples map to U5 tests. | confirmed | Plan:440–450. Their resource and signature limitations remain PA-1/L18/L20. |
| L76 | AE8 | consistency | A final miss remains a recorded failure and keeps release open. | confirmed | Plan:151,462,466. |
| L77 | Flows F1–F3 | consistency | Live diagnosis, stopped diagnosis and new queries have complete command paths in U3–U5. | confirmed | Plan:125–136,378,404,428; remote-host access is L66 and fresh-folder restart is explicit in F2. |
| L78 | Scope exclusions | consistency | The units avoid live caller queries, effectful handlers, upgrades, profiling and dashboards. | confirmed | Plan:160–186 versus units:344–470. No high-level approach replacement is needed. |
| L79 | Risks:326 | consistency | The risk estimate uses the same maximum failure-write size as KTD3. | refuted | Risk says up to 16 MiB, while KTD3 can write two separately bounded 16 MiB payloads, plus overhead. Correct write/disk/memory accounting in PA-2. |
| L80 | Assumption:311/R12 | codebase | An indented failure identifier can preserve all three stable event-line shapes. | confirmed | src/tools/serve.c:44 prints event lines separately from defect reports; plan keeps those lines unchanged. |
| L81 | Impact/Documentation | consistency | Guide claims are updated in each shipping milestone. | confirmed | Plan:102,334,381,407,431; final runbook update at :463 must also accurately describe new operator authority and retention. |
| L82 | Impact:317 | consistency | Diagnostic retention and operator clearing are promised in operational documentation. | confirmed | Plan:317. The clearing procedure is a U4/U6 documentation obligation; no new deletion command is implied. |
| L83 | Source paths | codebase | Cited existing implementation areas exist; newly proposed files are identified as future work. | confirmed | Read-only path inventory found all referenced source/current CI files; absent operator checker, introspection design, failure fuzzer and Phase4 artifacts are planned additions. |
| L84 | Verification/DoD | consistency | Checks are required rather than claimed to have already passed. | confirmed | Plan:474–498. This audit does not claim a build/test pass and corrects the mechanisms in PA-5/PA-6. |
| L85 | Stop conditions | consistency | The overview identifies gates for baseline ceiling, export size, replay and forbidden compatibility changes. | confirmed | Plan:18–22. Explicit deferred investigations are not newly discovered blockers merely because their measurements do not exist. |
| L86 | DoD:491/R23 | consistency | Removing abandoned experimental implementation need not delete required failed trial evidence. | confirmed | Plan:96 retains prompts/transcripts/gradings/failures; read :491 in that context and preserve fixture/evaluation evidence. |
| L87 | Diagram:259/KTD2 | consistency | The architecture diagram uses the selected operator-name spelling. | refuted | Diagram says mo.* whereas KTD2 selects mo-. Correct diagram alongside PA-4. |
| L88 | Formal-verification follow-up | codebase | Lean already proves Core type safety and determinism. | confirmed | formal/MoCore/Theorems.lean:25,170; formal/README.md:178. Preserve and reuse these proofs. |
| L89 | Formal-verification follow-up | codebase | The current Lean oracle does not evaluate service Simulation operations. | confirmed | formal/TEXT.md:35 explicitly stops at the outside-model boundary; this is not a service/crash-safety proof. |
| L90 | Formal-verification follow-up | codebase | Core fuel does not model the service VM's exact instruction budget. | confirmed | formal/TEXT.md:747 defines depth-bound fuel; ci/check_core_differential.py:9 counts resource cutoffs separately rather than comparing them. |
| L91 | Formal-verification follow-up | codebase | The lean CI group builds proofs and runs C-vs-Lean comparisons. | confirmed | ci/check.sh:379,407,420,440; Makefile:162 invokes the model build and oracle checks. No claim those checks were run in this audit. |
| L92 | Formal-verification follow-up | codebase | No existing TLA+ source/configuration was found in the inspected repository tree. | confirmed | rg --files -g '*.tla' -g '*.cfg' -g '*tla*' -g '!*.audit*.md' -g '!vendor/**' -g '!build/**' returned no paths. This is scoped search evidence, not a statement about external work. |
| L93 | Formal-verification follow-up | external | TLC can check finite state models of the proposed ordering protocols. | confirmed | [Official TLC documentation](https://docs.tlapl.us/using:tlc:start) and [model scope](https://nightly.tlapl.us/doc/model/model.html). Actual models, bounds and tool version must be defined; this does not prove the unmodeled C implementation. |
| L94 | Discussion scope | consistency | The user requested continued Lean proof work and TLA+ where useful. | confirmed | Current user message explicitly adds this requirement; PA-9 assigns proposed work without claiming completed proofs or checks. |

## Blind draft

**Excerpt used:** An agent operating a running Mo service should be able to inspect its behavior, answer questions its author supplied no tool for using exported state, and reproduce a saved failure. Fresh-agent trials against seeded failures should establish the result before v0.1. Investigate limited visibility, exclusively owned persistence and discarded failures. Preserve reference rules and Phase 3 compatibility; exclude upgrades, migrations, profiling, multicore/cross-process scheduling, dashboards and contribution-policy decisions. The audience is agents using MCP and the CLI.

**Approach:** Build inspection around the existing service loop and authenticated MCP server, with bounded results and CLI access. Reuse idle-only state access and the saved-value codec for a consistent typed snapshot that can be analyzed locally. Expose runtime facts already maintained internally, including queues, timers, supervision and committed sequence. Capture diagnostic artifacts before cleanup drops the execution and arguments, separately from the pruned recovery log. Replay offline with the original program, pre-state, handler, arguments, time and relevant execution conditions, and compare a defined outcome. Preserve commit-before-reply, exclusive store ownership, timer behavior and application-error distinctions. Evaluate usefulness with fresh agents, predetermined answers and reproduction criteria before release.

**Must-cover:**

- Concrete operator questions and the distinction between current state, retained history and unavailable facts: covered by R6/U1/U3; history sufficiency remains L67.
- Discovery, schemas, errors, names, pagination and machine-readable output: covered by U3/U5, with name normalization corrected in PA-4.
- Consistent export identity and complete private/recursive type coverage: export boundary covered by KTD1; missing explicit state-encoding work in PA-3.
- Respect live store ownership; bound export work, memory and disk impact: covered by KTD1/KTD4/U3/U5; ownership/capture gaps in PA-2 and L33, timing unknown in L15.
- Private-data authority, credential exclusion and artifact permissions: covered by R8/R9/KTD1; current bearer separation confirmed, publication scan requires definition.
- Diagnostic retention separate from the recovery log: covered by KTD3/U4; the user settled L21's interpretation, with reference clarification still required.
- Tool/timer failures, work/memory/size failures, application errors, admission refusal and uncertain store outcomes: assigned U4/U5; cause/resource work in PA-2/PA-8 and L11.
- Complete artifacts or explicit absence, capture refusal, eviction and snapshot survival: assigned U4; missing pre-store/resource refusal path in PA-2.
- Defined exact replay, program identity and diagnostic equivalence: assigned U1/U5; the user settled L18's reproduction criteria and L20/L38's replay/fix/state-query compatibility boundary. Artifact capture, identity fields and comparison checks remain pending; existing nominal identity is confirmed.
- Resource-sensitive replay rather than input-only determinism: R15 recognizes the issue; fix-check contradiction is PA-1.
- No production writes, network effects, normal replies, startup ticks or supervision from offline execution: covered by U5's isolated commands; factor existing validation rather than call service startup. Journal recovery is the explicitly qualified filesystem write.
- Compatibility, corruption, fault injection, crash boundaries and new-reader fuzzing: covered by U4/U5 and many planned tests; move fetch-reader coverage to U3 (PA-6), correct execution selection (PA-5).
- Fresh trials with fixed budgets/oracles, independent evidence and retained failures: covered by U1/U2/U6; the user accepted PA-7's pilot/freeze/independent-baseline design. L50/L52 still address freeze/oracle details.
- Guide and design claims limited to tested behavior: staged throughout U1/U3/U4/U5/U6; do not claim tests passed merely because their steps are listed.

**Draft assumptions:**

- The excerpt's goal was extracted from mixed sections and accurately represented the intended outcome; the parent had already seen solution choices, so independence is compromised.
- Export is sufficient for external analysis; a live query language is unnecessary.
- Exact replay concerns handler execution, not every network, OS or storage failure.
- Operator access to diagnostic data is intended, with precise confidentiality and retention rules still to define.
- The draft was based on static source inspection; no runtime success was assumed.

The drafting agent opened or content-searched only these repository files before returning its draft: `docs/reference/README.md`, `docs/design/services.md`, `docs/design/saved-state.md`, `docs/design/architecture.md`, `docs/design/implementation.md`, `docs/agent-guide.md`; `src/vm/vm.h`, `src/vm/service.c`, `src/host/store.c`, `src/heap/heap.h`, `src/base/limits.h`; `src/tools/protocol-server.h`, `protocol.h`, `protocol-value.c`, `protocol-descriptor.c`, `serve.c`, `cli.c`, `run.c`; `runtime/mcp.mo`, `runtime/mcp-rpc.mo`; `tests/test_service_store.c`, `tests/test_service_runtime.c`, `tests/test_protocol_server.c`, `tests/test_saved_state.c`. It read no plans, brainstorm/spec/ADR directories, audit files or commit messages. Parent parallel exploration likewise used ordinary source/design/test files; the contamination was the mixed plan excerpt described above.

