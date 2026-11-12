---
title: Phase 6, Intent and Autonomy - Plan
type: feat
date: 2026-11-12
topic: phase6-intent-autonomy
artifact_contract: ce-unified-plan/v1
product_contract_source: ce-brainstorm
execution: code
---

# Phase 6, Intent and Autonomy - Plan

## Goal Capsule

- **Objective:** A running Mo service accepts a change without a person only when everything Robert approved is still present and still holds, and every change that touches approved intent waits for him. Robert reaches a verdict, from simulation evidence and from real use of a service he relies on, on whether to let agents ship that way. The phase releases v0.3.
- **Means:** Promises that can describe a whole service and are checked in simulation, a strength measure for promises, approval records tied to exact content, and a review setting on the Phase 5 upgrade path.
- **Authority:** `docs/ROADMAP.md` (Phase 6 row), the language reference, `docs/design/services.md`, `docs/design/upgrades.md`, `docs/design/promise-strength.md` and `AGENTS.md` govern. Robert approves every change to Mo's language and toolchain surface: reference text, commands and error codes. The behaviour specified below is new; none of it exists yet.
- **Open blockers:** None before planning. The items under Outstanding Questions are answered during planning or when their milestone starts.

---

## Product Contract

### Summary

Phase 6 makes the upgrade door check each incoming program against what Robert approved. Each service gets a review setting: every change waits for Robert, or only changes to approved intent and migrations of stored state do. Before the door relies on intent, a promise can be written about a whole service and is checked in simulation, a strength measure shows whether promises reject wrong programs, and approved intent becomes readable in plain language and locked. The door is proven on the task queue sample, then runs on a service Robert uses, and the phase closes on his verdict.

### Problem Frame

Phase 5 gave an agent a way to change a running service. That path checks a second credential, compiles the incoming program, compares it with the running one and runs a migration when the state type changed. It does not consult the incoming program's promises, examples or tests. Whoever holds the credential can activate any program that compiles, including one whose promises were loosened until they passed.

Mo's intent forms stop at functions. A promise whose inputs include a `Simulation` is rejected with E1400, and the reference defers promises over a service's sequences. The task queue shows the result. Its 8 promises sit on functions in `services/task-queue/job-queue.mo`. Its behaviour as a service is covered by the 23 hand-written tests in `services/task-queue/queue-tests.mo`, which the agent changing the program can also change. `mo test` generates message sequences against a service only in its recovery check, and that check asks one question: does the state after a restart equal the state before the crash.

Nothing yet shows that a promise rejects a wrong program. `mo test --strength` is a design in `docs/design/promise-strength.md` and is not implemented. The first Mo toolchain (`robertguss/mo-lang`) ran ten control rounds in which no approved constraint tripped on a wrong edit. The agent that wrote its code also wrote the tests that judged the code, and re-pinned their expectations.

Robert therefore has two choices today: read every change in full, or trust a change on evidence that does not exist. `README.md`, the reference and the Phase 5 plan all assign approval of intent and the criteria for autonomous deployment to this phase.

### Key Decisions

- **The door has two review settings, and review everything comes first.** (session-settled: user-approved — chosen over leaving the review-everything stage to a later phase: without it the phase either grants autonomy Robert does not yet trust or changes nothing for him.) Governs R17, R18, R19.
- **People still read code.** The intent change is shown first and the code change stays available. (session-settled: user-directed — chosen over a flow in which Robert never sees code: Robert corrected that reading of the premise.) Governs R13, R21.
- **Simulation first, then a service Robert uses.** (session-settled: user-directed — chosen over proving the phase on the sample alone and over going live at once: reviews of real changes can earn trust that planted faults cannot, and simulation is the cheap place to find the door's defects.) Governs R1, R2, R23, R24.
- **Promises can be about a service.** (session-settled: user-directed — chosen over exercising the door in simulation with no new language feature: approved intent could otherwise state only function-level behaviour.) Governs R4, R5, R6, R7.
- **The phase closes on Robert's verdict, and either answer closes it.** (session-settled: user-directed — chosen over closing once the mechanism has run live, which leaves the phase's question unanswered, and over staying open until autonomy is achieved, which presses Robert to trust the system so the phase can end.) Governs R28.
- **Shadow runs, observation windows and automatic expansion are out.** (session-settled: user-approved — chosen over taking them up from Phase 5's deferral list: they were proposed in the `robertguss/mo-v2` archive and never approved by Robert, and the two review settings answer the trust question without them.)
- **Any edit or removal of approved intent goes to Robert.** An edit includes a change to anything the approved item reads. The door does not judge whether an edit weakens intent, because a wrong judgment would let a loosened promise through. Governs R14, R19.
- **Approved intent is promises and example tables.** Tests stay the agent's to change. Governs R11.
- **Evidence comes before reliance.** Intent that the strength measure shows to be weak is strengthened before the door relies on it, and the wrong versions that measure it come from a session that does not write the intent. Governs R9, R10, R31.
- **The lock's limit is documented and left open.** An agent with a shell on the service's own machine can go around the door, because Mo has no operating-system confinement, and this phase does not add it. Governs R15, R30.
- **In the live stage, agents get no shell on the service's host.** They reach the service only through its endpoint. (session-settled: user-approved — chosen over giving agents the host and saying in the verdict that the door could be bypassed: the planted change of R27 and the verdict would otherwise come from a door the agent could go around.) Governs R25, R27, R30.
- **An upgrade that migrates stored state waits for Robert in this phase.** (session-settled: user-approved — chosen over letting it activate alone when no approved item changed: simulation never runs on the live service's stored data, and there is no rollback.) Governs R19, R32.
- **Limits are fixed only when the live service hits them.** Everything else waits for Phase 7. Governs R29.

### Requirements

**Phase shape**

- R1. The phase runs five milestones in order: M1 promises about a service; M2 promise strength; M3 readable intent and the lock; M4 the review door, proven on the task queue; M5 the live stage and the close.
- R2. No service Robert uses runs behind the door before the M4 proof (R23) exists.
- R3. The phase releases v0.3 when it closes per R28.

**Promises about a service (M1)**

- R4. An author can write a promise about a service's behaviour across a sequence of messages, with a sentence, as function promises have.
- R5. `mo test` checks such a promise against generated sequences of tool calls, clock advances, crashes and restarts, and reports a failure as a short sequence that still fails.
- R6. The same program and promise give the same result on every run.
- R7. The task queue states its service guarantees as promises, and they pass.

**Promise strength (M2)**

- R8. `mo test --strength` implements the parts of `docs/design/promise-strength.md` that R9 and R10 need, and covers the service-level promises of R4. Planning names those parts. The rest of that design is deferred.
- R9. The strength of the task queue's intent is measured and recorded before M4 begins.
- R10. Before M4 begins, the task queue's intent is strengthened until Robert accepts its strength result.
- R31. The wrong versions and planted changes used for R9, R10 and R23 are written by an agent session that does not write the task queue's intent, and each record names the session that wrote it. The wrong versions are frozen before that intent is strengthened. Planted changes that edit approved intent are frozen before the M4 proof runs.

**Approved intent and the lock (M3)**

- R11. Approved intent is the promises and example tables Robert has approved, service-level promises included. Tests are not approved intent.
- R12. Robert can read a program's promises and example tables, each marked approved or not, without reading handler code.
- R13. For a proposed change, Robert sees each approved promise and example table that changed, before and after, ahead of the code change. A change to a declaration an approved item reads (R14) is shown with that item.
- R14. An approval covers exact content: editing or removing an approved item leaves it unapproved until Robert approves the new content. An item's content includes every declaration its conditions, anchors and example rows read, directly or through one another: constants, helper functions and types. The function or service the item is about is not part of it.
- R15. Only Robert can create or change an approval, and he does so somewhere an agent working on the program cannot act as him.
- R16. An agent can add promises and examples without approval, and they stay unapproved until Robert approves them.

**The review door (M4)**

- R17. Each service has a review setting with two values, review everything and review intent changes only. A service starts on review everything, and only Robert changes the setting.
- R18. Under review everything, every upgrade waits for Robert's yes. Each upgrade's record says whether review intent changes only would have activated it without him, and what he answered.
- R19. Under review intent changes only, an upgrade that leaves every approved item unchanged and carries no migration (R32) activates without a person, and an upgrade that edits or removes an approved item (R14) waits for Robert's yes.
- R20. Under either setting, an upgrade activates only when the incoming program passes its own promises, examples and tests. One that fails is refused and the running program continues.
- R21. A waiting upgrade shows Robert the intent change (R13) first and the code change after, and the running program keeps serving while it waits.
- R22. Robert's yes applies only to the incoming program he was shown, on the running program it was compared with. It approves that program's changed intent and activates it. When either program has changed since he was shown, nothing activates and he is asked again. His no discards the upgrade and changes nothing.
- R23. Before any live use, the door is exercised many times on the task queue with planted changes of three kinds: fixes that leave approved intent unchanged, changes that edit or remove it, and wrong versions from the M2 measurement (R9) that leave it unchanged. Every change of the second kind waits, and under review intent changes only no change of the first kind does. The door refuses every wrong version that the strength measure counts as rejected.
- R32. In this phase, an upgrade that carries a migration of stored state waits for Robert's yes under both settings.

**Live stage and close (M5)**

- R24. A Mo service Robert uses runs behind the door on its own host, starting on review everything.
- R25. Agents make the changes to that service, and every change goes through the door. Agents reach the service only through its endpoint, over a tunnel limited to that port, with no shell on its host and no access to its data folder. Restarting the service on a new program, and starting it in a fresh data folder, are Robert's alone.
- R26. When Robert rejects a change that passed every check, a promise or example that would have caught it is added before the next change is proposed.
- R27. At least one planted change to approved intent is pushed at the live service, under the access R25 allows, and the door holds it.
- R28. After a minimum of real use, Robert gives a verdict on moving the service to review intent changes only, and either answer closes the phase. The record of R18 is part of the evidence he weighs. After a yes, one real change activates without a person. After a no, what is missing is recorded as the next work.
- R29. A speed or size limit is fixed in this phase only when the live service hits it.

**Stated limits**

- R30. The agent guide and the design documents say what the lock does not protect against, including an agent with a shell on the service's own machine. The agent guide states the access rule of R25 for a service behind the door.

### Actors

- A1. Robert: approves intent, answers waiting upgrades, sets each service's review setting and gives the verdict.
- A2. Agent: writes and changes Mo programs, adds intent, pushes upgrades and fixes failures in the live service.
- A3. Upgrade door: the service's upgrade path. It compares an incoming program with the approvals, then refuses, holds or activates.
- A4. `mo test`: checks promises, including service-level ones in simulation, and measures strength.

### Key Flows

- F1. A fix that leaves approved intent alone
  - **Trigger:** A2 pushes an upgrade to a service on review intent changes only.
  - **Actors:** A2, A3
  - **Steps:** A3 finds every approved item unchanged and no migration. The incoming program passes its checks. A3 activates it.
  - **Outcome:** The fix is live and no person was involved.
  - **Covered by:** R19, R20, R32
- F2. A change that touches approved intent
  - **Trigger:** A2 pushes an upgrade that edits or removes an approved item.
  - **Actors:** A1, A2, A3
  - **Steps:** A3 holds the upgrade. A1 reads the intent change, then the code change if he wants it. A1 answers.
  - **Outcome:** A yes activates the upgrade and approves the new content. A no discards it. The running program serves throughout.
  - **Covered by:** R14, R19, R21, R22
- F3. Review everything
  - **Trigger:** A2 pushes any upgrade to a service on review everything.
  - **Actors:** A1, A2, A3
  - **Steps:** A3 holds the upgrade. A1 reviews it. When A1 rejects a change that passed every check, A2 adds intent that would have caught it.
  - **Outcome:** Nothing activates without A1, and each such rejection leaves the service with more intent than before.
  - **Covered by:** R18, R21, R22, R26
- F4. First approval
  - **Trigger:** A program carries promises or example tables Robert has not approved.
  - **Actors:** A1
  - **Steps:** A1 reads them. A1 approves the ones he accepts.
  - **Outcome:** Those items are approved intent and the door protects them.
  - **Covered by:** R12, R14, R15, R16
- F5. The verdict
  - **Trigger:** The live service has had its minimum of real use.
  - **Actors:** A1
  - **Steps:** A1 weighs the strength results, the M4 proof and the record of his reviews (R18). A1 answers.
  - **Outcome:** The phase closes on either answer.
  - **Covered by:** R28

### Acceptance Examples

- AE1. **Covers R19, R20.** Given the task queue on review intent changes only with approved promises, when an agent pushes a handler fix that changes no approved item, carries no migration and passes its checks, then the fix activates and Robert is not asked.
- AE2. **Covers R14, R19, R21.** Given the same service, when a push rewrites an approved promise's sentence, changes its body or deletes a row of an approved example table, then the upgrade waits, Robert sees that item before and after, and the old program keeps serving.
- AE3. **Covers R20.** Given approved promises that a push leaves unchanged, when the incoming program fails one of them, then the upgrade is refused under either setting and Robert is not asked.
- AE4. **Covers R18.** Given a service on review everything, when a push changes no approved item and passes its checks, then it still waits for Robert, and its record says the other setting would have activated it.
- AE5. **Covers R16, R19.** Given a service on review intent changes only, when a push only adds a new promise, then it activates without Robert and the new promise shows as unapproved.
- AE6. **Covers R15.** Given an agent that holds the service token and the upgrade credential, when it tries to mark a changed promise approved, then the attempt fails and the item stays unapproved.
- AE7. **Covers R4, R5.** Given a service-level promise on the task queue and a planted fault that breaks it only after a crash and restart, when `mo test` runs, then it fails and reports a short sequence that contains the restart.
- AE8. **Covers R26.** Given a service on review everything, when Robert rejects a change that passed every check, then the next proposed change carries a promise or example that the rejected change fails.
- AE9. **Covers R28.** Given the minimum of real use is met, when Robert's verdict is no, then the gaps he names are recorded as the next work and the phase closes with the service still on review everything.
- AE10. **Covers R14, R19.** Given the task queue on review intent changes only with the promise "a job is attempted at most three times" approved, when a push changes only the constant that promise reads, then the upgrade waits and Robert sees the constant before and after.
- AE11. **Covers R32.** Given a service on review intent changes only, when a push carries a migration of stored state and changes no approved item, then it waits for Robert.

### Scope Boundaries

- Shadow runs, observation windows and automatic expansion.
- Operating-system confinement of agents on a service's host.
- Judging automatically whether an edit weakens intent.
- More than one approver, roles, or delegated approval.
- Checking promises against a live service's recorded messages.
- A declared agent evaluation campaign, of the kind that closed Phases 4 and 5, as a condition of closing.
- Rollback after activation. Phase 5's rule stands: fix forward.
- Speed and size work that the live service does not need (R29).
- The parts of the promise-strength design that the task queue's measurement does not need (R8).
- The choice of the live service. It is chosen when M5 starts.

### Dependencies and Assumptions

- The door extends Phase 5's upgrade path: `mo push` on the host, the `mo-upgrade-open`, `mo-upgrade-page` and `mo-upgrade-commit` tools remotely, a second credential, and a swap between two messages.
- That path has no abort command. A refused upgrade leaves the old program running, and an open upgrade expires after 60 seconds idle (`docs/design/upgrades.md`). A wait for a person lasts longer than that.
- `Simulation` already runs a service on an in-memory store with a fake clock, crashes and restarts. Generated sequences exist only inside the recovery check.
- `docs/design/promise-strength.md` does not mention services.
- The task queue's earlier deployment is gone, so the live stage needs a new host.
- `docs/design/sandbox-properties.md` records that write access to a service's data folder equals upgrade authority. That bounds what R15 can promise.
- Assumed, not verified: the upgrade path can run the incoming program's checks within a pause or wait that callers tolerate.
- Assumed, not verified: an agent can diagnose and fix the live service through its endpoint alone (R25). The agent guide today has agents push, restart, fetch, replay and query on the service's host.

### Outstanding Questions

**Resolve Before Planning**

None.

**Deferred to Planning**

- Does the phase open with a design step that Robert approves before code, as Phases 4 and 5 did?
- What does a service-level promise look like in source, and which reference rules change (R4)?
- Which parts of the promise-strength design do R9 and R10 need, and does it extend to service-level promises as written (R8)?
- Does the strength result Robert accepts need wrong versions the strengthening session has not seen (R10, R31)?
- What strength result does Robert accept (R10)? He decides when the first measurement exists.
- Where does Robert approve, and what keeps an agent from acting as him there (R15)?
- How does Robert learn that an upgrade is waiting, and how does he answer it (R21, R22)?
- Where do the incoming program's checks run when an upgrade arrives, and what bounds their time (R20)?
- How long may an upgrade wait, and what happens to it when the service restarts (R21)?
- What happens to other pushes while one upgrade waits for Robert? The path has one upgrade slot per service, so a waiting upgrade either blocks an unattended fix or can be replaced (R19, R22).
- Where are the review setting and the approvals kept, so that neither a writer of the data folder nor a holder of the upgrade credential can change them (R15, R17)?
- How many planted runs make the M4 proof (R23)?
- Which agent diagnostics need the service's host today, and how does each work through the endpoint alone (R25)?
- What is the minimum of real use (R28)? It is set when M5 starts.

<!-- ce-section: work-relationships -->
### How This Work Fits Together

This plan covers Phase 6 of `docs/ROADMAP.md`. The breakdown below is the current understanding, not a committed roadmap.

- Phase 5, finished: Enables this phase by supplying the upgrade path the door extends.
- Phase 7, Fast: Can proceed independently of M1 to M4, and may start alongside M5, which is mostly waiting. Shares the limits the live service hits (R29).
- Phase 8, Ecosystem: Still to decide whether packages share approved intent in place of source, as direction `d34` in `robertguss/mo-lang` proposed. If they do, that work Depends on R11 to R16.

### Sources

- `docs/reference/README.md`, lines 1267 to 1351: `Simulation`, E1400 and the recovery check.
- `docs/design/upgrades.md`, `docs/design/services.md`, `docs/design/promise-strength.md`, and the Gaps section of `docs/design/sandbox-properties.md`.
- `docs/plans/2026-11-08-0011-feat-phase5-evolves-while-running-plan.md`: Phase 6 owns approval, and the list of what Phase 5 deferred.
- `services/task-queue/`: the sample service, with 8 promises and 27 tests.
- `robertguss/mo-lang`, `mo-wiki/directions/d19` to `d21`: people state constraints, agents write them down, and a person sees a change only when it removes or weakens one. `mo-wiki/state-of-the-project.md`: the ten control rounds.
- `robertguss/mo-v2`, `docs/DECISION-MAP.md`: what Robert trusts must be readable by him and checked by something the code-writing agent cannot quietly change. `docs/research/documents/glossary/rules-waivers-and-approval.md`: pinned approval and approval fatigue.
