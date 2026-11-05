# Prospective U6 diagnostic evaluation protocol

**Protocol design and defaults approved by Robert on 2026-11-05; not yet frozen or authorized for scored collection.**
Approval provenance: Robert replied “Approved” in this authoring conversation after delivery of the reviewed local draft. This approves the concrete design defaults below, not still-unspecified artifacts, implementation readiness or session execution.
This is early protocol design under the [canonical plan](../../docs/plans/2026-11-03-1704-feat-phase4-see-inside-plan.md), R20–R25, R28 and KTD6. It implements no tooling and authorizes no sessions, fixtures, publication or release. U3 is merged per Robert. The U7 → U8 → U9 → U4 → U5 chain must finish before scored U6 collection. Names below describe required behavior; final command support and recorder compatibility remain unverified until those milestones deliver evidence.

## Question and evidence boundary

Can fresh agents obtain supported diagnostic answers and check a reconstructed failure using the completed instruments, while accurately identifying unavailable evidence? This is a capability-and-limitations evaluation, with no estimate of improvement, speedup or causal instrument effect. Ordinary source inspection, tests and simulation remain available. Instrument use and task correctness are separate outcomes.

The [public findings](pilot/findings.md), [ledger](pilot/ledger.md) and [historical protocol](protocol.md) remain unchanged: 30 attempts, 29 completed successful harness grades, pilot 7 operator-interrupted; four bug variants uncalibrated at the cap. Hardening changed reports only; three final report revisions were unpiloted and the fourth was solved in pilot 30. Pilot 3 has recovered provenance with unknown fields. No independent baseline exists. Historical predictions remain historical predictions, including misses; the mandatory 36-session prerequisite is superseded.

A comparative claim would need a separately approved prospective independent baseline, equivalent grading, a sample-size/comparison justification and declared model-unavailability policy under R22. Neither these sessions nor the historical pilots supply that baseline.

## Proposed allocation and scenarios

Default: 24 fresh sessions, four scenario cells × two client/model families × three independent session starts. Each cell uses one independently validated scenario instance; repetitions measure variation across fresh sessions, not independent incident diversity. Report that narrow scope. This allocation gives every required workflow six opportunities and both clients three per cell without pretending to power a comparison. No optional stopping or difficulty-based replacement.

| Cell | Diagnostic questions | Required evidence and coverage | Correctness criteria |
| --- | --- | --- | --- |
| A: live incident | What is true now about state, pending work and supervisor activity? Which recent committed operation explains an observed wrong state, if the retained tail supports that claim? | Authenticated overview, state, mailbox, timers, memory/peak, supervisor and committed-history views; observation times/sequences and truncation flags. Export when a view is incomplete. | Match independent snapshot facts; distinguish queued work from committed work and handler failure from application Error/admission refusal. A causal attribution needs the retained ordered transition, not merely plausible source code. |
| B: new state question | What cross-record aggregate or private-state fact at this snapshot is absent from every author tool? | Independent audit of author-tool schemas, outputs and supported compositions/pagination establishes that the required information is not exposed; immutable state export identity and complete fetch; pure query source and result using the defining module or exposed interface. | Answer matches independently computed state oracle at the specified snapshot. It need not be impossible to infer manually; the question must lack an author-tool answer. No source hiding or opaque naming. |
| C: retained failure and repair | What failed, with which logical inputs and budget? Does the original reproduce, and does a compatible repair satisfy the application contract? | Retained record, original program/build, pre-call state, ordered arguments/time, structured signature; original replay then fix check with both outcomes/identities. | Original signature reproduces; compatible revision succeeds under recorded limits and saved-state check; independent application tests pass. Runtime verification alone cannot establish application correctness. |
| D: retention boundary | What can still be established after snapshot/retention turnover, and what cannot? Is a proposed revision actually verified? | Stopped-folder history without service startup; explicit snapshot/tail boundary, retained or unavailable record status; original-first result or refusal, including missing provenance. | Supported present facts are correct; missing causal history is explicitly unknown. A successful revision with unreproduced original is unverified; absent record/build cannot be silently reconstructed or substituted. |

Future scenario authors must specify realistic workload sizes, chronological calls and arrival times, snapshot position, retained record identities, and expected eviction/refusal behavior before approval. Cell C uses a deterministic defect or work-budget exhaustion; use one of each across later separately declared experiments if needed, not adaptive swapping here. Cell D includes an ordinary snapshot that removes relevant causal calls and a retained failure or explicitly missing record. Do not assert that snapshots themselves delete retained failures. Exercise count/byte retention through realistic repeated incidents under the implemented deterministic policy, with independent expected survivors. No obfuscation, concealed source, artificial puzzles or changed application semantics to manufacture difficulty.

Live heap/mailbox pressure remains an implementation regression, outside scored scenarios unless a later approved protocol establishes reproducibility. Its original-not-reproduced behavior must nonetheless appear in the U5 evidence reviewed before collection. The bounded fix claim never establishes fitness under omitted live allocations.

## Expected outcomes and grading

Before any scored session, a scenario author writes expected state facts, ordered transitions, signatures, answers, retention boundaries and application tests from the reference/application contract. An independent reviewer, who did not author the scenario or solve the sessions, checks these against the prepared inputs. Keep signed-off hashes privately. Check that the original fails the intended application test and the reference repair passes; include an incorrect repair that passes replay but violates application behavior. This is prospective preparation, not work authorized by this draft.

For each session retain three independent fields:

- **Task correctness:** pass only if all cell-specific facts and required repair tests pass, and every causal/verification claim is supported. Missing final answers fail. Correctly saying “unknown” passes only where the frozen oracle says evidence is insufficient; blanket abstention fails supported questions.
- **Instrument coverage:** each required instrument is `observed-success`, `observed-refusal`, `observed-incomplete`, `not-used` or `record-missing`, with ordered evidence references. Invocation alone is not successful coverage. A designed refusal counts as coverage only when its reason matches the frozen expectation.
- **Record validity:** complete, incomplete or unavailable, independently of correctness. Keep content-only grades for diagnosis even when record invalidity prevents acceptance.

Two graders independently assess the factual/causal rubric, blinded to client identity where practical and never using the solving model's self-grade. Mechanical checks judge structured answers and application tests. Record both initial grades and evidence citations; an independent adjudicator resolves disagreement before aggregate reporting. An unresolved disagreement remains non-passing for the gate. Name graders and conflicts before collection. No grading rule is changed after an answer is seen.

Cross-check every applicable original-first verdict against the independent application oracle and expected reconstruction signature. Record disagreements as findings, not automatic proof that either oracle is right. Distinguish `reproduced`, `different`, `not_reproduced`, `refused`, `verified`, `not_fixed` and `unverified` according to the delivered contract. Wrong identity/schema/argument order is a refusal, not an ordinary failed repair. Fix checking requires original build/program identity, compatible revised state and ordered argument/callable/result shapes, recorded limits and saved-copy size validation.

## Sessions, budgets and clients

Default session cap: 60 minutes from verified task delivery, with setup time recorded separately. No intervention, hints or extra time after exposure. Maximum scored agent time is 24 hours, sessions sequential to avoid workload contention. No token or call-count cap; record provider usage, calls and elapsed time descriptively, not as improvement evidence. Robert approves a monetary ceiling before scheduling; reaching it stops collection with missing slots, never drops costly observations.

Use the release candidate executable and exact source/guide hashes, local host access, fresh isolated accounts/workspaces, source, ordinary author tools, logs and a consistent stopped-folder copy. All supported Phase 4 instruments are allowed. The prompt states incident questions and required evidence, not a solution recipe. No earlier conversation, project agent instructions, other sessions, private keys, fixture generators or grader access. Record the exact prompt and available instructions.

Proposed client defaults retain the public pilot configurations to reduce new harness uncertainty: Claude Code 2.1.317 with `claude-opus-5-5`; Codex 0.166.0 with `gpt-6.1-sol`, `xhigh`. These are proposals, not verified current availability or immutable weights. This document's authoring configuration (Astra medium) is not a scored client allocation. Explicitly pin every setting and auxiliary model in the approved configuration; default is no delegated solving, no browsing and no persistent memory. If a client invokes auxiliary models despite that setting, retain the attempt as a configuration violation.

Reuse the harness's ephemeral/event-stream modes, strict Claude MCP config with allowed MCP tools, and Codex explicit MCP default approval with approval policy `never`; author and operator calls must both be usable. Record sandbox, network policy, environment/config hashes, client binary/version, model identifier and reported revision, reasoning setting, date, approval settings and full command. No silent user-config inheritance. Provider aliases may change weights; claims are limited to recorded configurations and dates.

Runtime budgets are separate from agent wall time: freeze implemented named work/heap/stack/depth/storage/history/export limits and actual values. Replay uses recorded budgets without increases; queries/history use frozen defaults. The design specifies 16 MiB saved state, 8 KiB export pages, 60-second idle pin expiry and 64 MiB history read bound; verify delivered values rather than assume implementation. Preflight estimated roughly 103 seconds for a 16 MiB export at 20 calls/second, so export time belongs inside the hour. Log refusal, truncation, replacement and expiry; never silently raise limits to finish a session.

## Recording and unavailable data

Reuse `harness.py` isolation, artifact hashes, service lifecycle and attempt manifests; `proxy.py` ordered MCP traffic and bidirectional call matching; `grade.py` independent hidden tests and answer extraction where applicable; `scan.py` credential checks. Their current success boolean combines application grading, record matching and harness failures and cannot stand in for the three separate outcomes above. Diagnostic rubrics, offline-command transcript completeness, auxiliary-model enforcement and task-exposure detection require prospective validation; they are not claimed supported today. No code changes are made here.

Keep complete client streams, ordered MCP requests/responses, command inputs/outputs/exits, original and revised source, query text, artifact/build identities, service logs, runtime budgets, private grading evidence and all attempts. The public recorder's shell host check does not inspect scripts written and then executed; audit those commands independently. Unknown metadata stays unknown, not inferred as successful provenance.

Assign every slot and a deterministic shuffled order from seed `20261105` before launch. A failure provably before task delivery may retry the same slot at most twice within seven days; retain all attempts. First assistant event alone does not prove absence of earlier task exposure. If exposure is uncertain, treat it as exposed. After exposure, timeout, interruption, configuration error, provider failure, lost evidence or absent answer consumes the slot and is non-passing, with its reason distinguished from diagnostic error. No replacement.

Unavailable pinned client/model: do not substitute. Use the same pre-exposure retry rule; afterward mark the slot unavailable. Keep the planned denominator of 24 and also report exposed/completed denominators. Missing slots cannot satisfy the gate. Changing model, scenario, rubric or budget after collection begins requires a new named prospectively approved experiment; retain the previous experiment without pooling it into the new one.

## Proposed acceptance gate and claims

Default acceptance requires all of the following; these thresholds are proposed operational coverage criteria, not statistical population guarantees:

1. All 24 planned slots have complete valid records; at least 20/24 pass task correctness, at least 4/6 in each cell and at least 9/12 in each client family. This permits a few ordinary diagnostic misses while preventing pooled success from hiding a failed workflow or client.
2. Every listed instrument/behavior has successful or correctly expected-refusal evidence in both client families. At least 2/3 sessions in every cell/client combination satisfy that cell's full coverage requirements. Task correctness does not compensate for absent instrument evidence.
3. Zero unsupported causal assertions about missing history or false verified-fix assertions in any session final answer, even when graders correctly reject them. Exploratory hypotheses explicitly withdrawn before the final answer do not trigger this rule. Separately require zero tool-level false verified verdicts and zero unresolved disagreements between tool verdicts and independent oracles. These approved blockers apply even if aggregate scores pass.
4. U3/U7/U8/U9/U4/U5 completion and the canonical verification obligations have actual evidence: Lean properties and their assumptions, finite TLC checks/configurations, scoped CBMC bounds, reader fuzzing, runtime/fault/crash checks and negative controls. Skips, missing tools, waivers and exclusions are visible and cannot count as passes. Session grades do not replace this evidence.

Report exact counts by cell/client, all failures and coverage categories. No causal or population generalization from three repeats of a fixed incident. Below-gate outcomes leave U6 open; do not tune thresholds retrospectively. Even a passing gate only supports Robert's separate release decision under R25. Release documentation, tagging and publication are outside this draft's authority.

## Observed material excluded from scoring

The public preflight sizing/growth runs, query prototype, history trace, synthetic harness tests and client-name/approval probe are development or feasibility evidence. The query prototype rewrote image keys and is not proof of supported query behavior. The client probe used proxy-supplied operator responses, not the final production implementation. Native-client smoke establishes discovery/routing/access only; later U7–U9 smoke and deterministic fixtures must be inventoried by commit, date and artifact before freeze, separately from scored evidence. None can enter the evaluation retrospectively. This draft neither inspects nor copies private pilot variants or answer keys.

The [preflight record](preflight/README.md) demonstrates why complete exports and retention-aware answers matter: large state outgrows replies, and a snapshot leaves current state while losing the earlier causal calls. Its measurements had no prior predictions and are not implementation guarantees. The [design](../../docs/design/introspection.md) supplies interfaces; if its prose and the canonical compatibility contract disagree, resolve that discrepancy before freeze rather than silently selecting weaker compatibility.

## Approval and freeze checklist

Robert approved the capability-only scope, four cells, 24-slot allocation, 60-minute cap, retry/unavailability policy, proposed client defaults, independent grading rubric and numerical/coverage/zero-false-claim gates on 2026-11-05. Earlier uses of “proposed” describe the defaults presented for that approval; they do not establish availability or a completed freeze.

Still required before scored collection:

- Exact private scenario identities, expected outcomes, realistic ordering/retention and independently reviewed keys, prepared only under separate authorization.
- Verified available client/model configurations, actual runtime limits and a monetary ceiling; any replacement configuration needs prospective approval.
- Named independent graders and adjudicator.
- Completed U7 → U8 → U9 → U4 → U5 tooling and verification evidence.
- Robert's approval of final immutable protocol/prompt/config/scenario/grader/build/guide hashes and shuffled slot manifest, followed by explicit authorization to begin scored collection.

This approval is not a freeze record. Record final approval provenance/date and hashes before any exposure. Record new predictions before checking them and obtain the Working Agreement's independent check for guarantee claims. Preserve failed predictions and every attempt. Private originals stay private; publication, if later authorized, requires scanning the actual selected artifacts and supported encodings for all known credentials (expired included), seeded-leak scanner checks and failure on unreadable files. Never commit private defects, fixtures, keys, reports or credentials. No publication or external messages are authorized here.
