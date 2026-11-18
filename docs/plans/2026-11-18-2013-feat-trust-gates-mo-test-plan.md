---
title: Trust Gates in mo test - Plan
type: feat
date: 2026-11-18
topic: trust-gates-mo-test
artifact_contract: ce-unified-plan/v1
product_contract_source: ce-brainstorm
execution: code
---

# Trust Gates in mo test - Plan

## Goal Capsule

- **Objective:** Robert can rely on a Mo program that an agent wrote without reading its code. A passing `mo test` means the program's checks notice a change to its code and its public surface states what it is for. Each change comes with an account of what it altered, in terms of checks.
- **Means:** Three additions to `mo test`: the mutation gate, a rule that every exposed function and service tool has a promise or an example, and the change summary. A measurement on the task queue shows how far to trust the gate.
- **Authority:** `docs/reference/README.md` and `AGENTS.md` govern. Robert approves every change to Mo's language and toolchain surface: reference text, commands and error codes. `GLOSSARY.md` holds the vocabulary this plan uses: check, mutant, wrong version, survivor, mutation gate, behaviour change and change summary.
- **Open blockers:** None.

---

## Product Contract

### Summary

`mo test` gains three things that work as one. It fails while any mutant of the program survives. It fails when an exposed function or a service tool has no promise and no example. Given a previous version of the program, it prints a change summary.

The work includes fitting the repository's own Mo programs to the gates, and a measurement that compares the mutation gate against the agent-written wrong versions of the task queue.

### Problem Frame

Robert writes none of the Mo code and reviews little of it. His trust in a program rests on its checks, and by default nothing measures whether those checks would notice a bug. A program can pass `mo test` with checks that pin almost nothing.

Phase 6 measured this once. The task queue's intent killed 3 of the 12 agent-written wrong versions in the working half, and six of the nine survivors changed only handlers that no service promise judged (`docs/status/phase6-run.md`). Strengthening the intent by hand raised that to 11 of 12, and the held-back half of 12 then scored 11 as well. The measure that found the weakness is a report an agent runs on request, against wrong versions someone must write first. It does not stop weak checks from passing.

Trust also builds over time, change by change. Today a change arrives as a diff of code. Nothing describes it in terms Robert can judge without reading that code.

### Key Decisions

- **A surviving mutant fails `mo test`.** A report can be ignored and a gate cannot. (session-settled: user-approved — chosen over a kill-rate report as `docs/design/promise-strength.md` gives for wrong versions: a report leaves weak checks able to pass) Governs R1, R2.
- **Tests count as kills.** Effects such as what `main` prints are checked only in tests today, so a gate limited to promises and examples could not pass for programs that mostly act. (session-settled: user-approved — chosen over counting only promises and examples: that would need a new form for stating effects first) Governs R3.
- **Tests stay the agent's to change.** The mutation gate already fails when a test is loosened far enough to let a mutant through. (session-settled: user-approved — chosen over locking tests the way promises are locked: every refactor would wait for Robert, and outside a running service there is nowhere to hold an approval)
- **Outside a running service Mo stores nothing that a later run relies on.** Anything Mo writes on a machine where agents run, an agent can rewrite, so a stored result would be a claim and not evidence. (session-settled: user-approved — chosen over a store for each program that only a credential held elsewhere can change: the review door that would be its model has not yet been used for a real approval) Governs R11, R12.
- **Plain `mo test` tries every mutant, and naming a previous version shortens the run.** The rigorous run needs nothing but the program. (session-settled: user-directed — chosen over a full run every time: a gate that takes hours is too slow to use while writing code) Governs R8, R9, R10, R28, R29, R30.
- **The first version has no exceptions.** An exception is where an agent that wants a pass would go first. (session-settled: user-approved — chosen over exceptions written in the source with a reason: nobody has yet seen which mutants of a Mo program cannot be killed) Governs R7.
- **An exposed function or service tool with no promise and no example fails `mo test`.** Promises and examples are what Robert can read and approve, and the task queue's survivors sat where there were none. (session-settled: user-approved — chosen over leaving it to the mutation gate: tests alone can kill every mutant and leave nothing readable) Governs R13, R14.
- **The change summary is what Robert reads for a change.** It describes a change without showing code. (session-settled: user-approved — chosen over reading the diff or having no fixed review surface) Governs R15, R16, R17.
- **A rename that hides a behaviour change is a stated limit of the change summary.** Robert reads the checks that could not be compared as well as the ones no longer true. (session-settled: user-approved — chosen over holding such a change until the agent restates the old checks: the smaller fix keeps the summary a report) Governs R15.
- **A run of selected checks is never a pass of the gates.** It tries no mutants and says so. (session-settled: user-approved — chosen over trying every mutant on a selected run: a quick run of one check would cost a full mutation run) Governs R7, R27.
- **The repository's own programs are fitted as part of this work.** A gate that is on by default fails every program that has not met it. (session-settled: user-approved — chosen over turning the gates on for new programs only) Governs R18, R19.
- **The proof is a measurement against wrong versions, with no pass mark.** Mutants are small mechanical changes and wrong versions are mistakes an agent thought plausible, so one can check the other. (session-settled: user-approved — chosen over relying on the gate unmeasured) Governs R21 to R25.
- **Complexity and duplication gates wait for evidence.** A crude count found 207 functions with a median of 7 lines of code and one function over 30 lines. (session-settled: user-approved — chosen over adding a complexity limit and a duplication check now: a limit needs a number and the problem is not showing up)

<!-- ce-section: work-relationships -->
### How This Work Fits Together

This plan covers the gates in `mo test` and their proof. The rest of this list is the current understanding of the surrounding work, not a committed roadmap.

- **Phase 7's M3, the HTTP client.** Depends on this plan: the client is written under the gates from its first line and is their second trial. M1 has merged and M2 is in progress; neither depends on this plan. Phase 7 is planned in `docs/plans/2026-11-17-1900-feat-phase7-outside-world-plan.md`.
- **The gate at the review door.** Depends on this plan and gets its own. The door would refuse a push when a changed function has a surviving mutant, comparing against the running program. Its time limit is set from this plan's measurements.
- **Trying every call sequence for a service.** Still to decide. This plan's proof counts the survivors that only a rare sequence could kill, and that count is the case for or against it.
- **Exceptions for mutants that cannot be killed.** Still to decide. This plan's proof counts them.

### Actors

- A1. Robert. He reads the change summary, approves reference text and merges pull requests.
- A2. An agent writing or changing a Mo program. It runs `mo test` and acts on what it reports.
- A3. The `mo` toolchain. It makes the mutants, runs the checks and reports.

### Requirements

**The mutation gate**

- R1. `mo test` makes the mutants of a program itself. Each mutant is a copy of the program with one small change to the code of one function in the program's own modules. A library module has its mutants made when `mo test` runs on that module.
- R2. `mo test` fails while any mutant survives.
- R3. A promise, an example or a test can each kill a mutant, and the result says which kind of check killed it.
- R4. The toolchain never changes a promise, an example or a test to make a mutant.
- R5. A mutant on code that no check runs survives.
- R6. For each survivor the result names the function, the line and the change that no check noticed.
- R7. Neither the program's source nor an option of `mo test` can excuse a single mutant.
- R8. `mo test` with no previous version named tries every mutant of the program.

**The shorter run**

- R9. When the caller names a previous version of the program as a folder, `mo test` tries only the mutants of functions that changed, or whose checks changed, since that version.
- R10. The result of a shorter run says how many functions it carried over untried and gives the digest of the previous version.
- R11. Outside a running service's data folder, Mo stores nothing that a later run relies on. A cache may make a run faster, and a run without it gives the same result.
- R12. The toolchain does not read git.

**Stated intent**

- R13. `mo test` fails when an exposed function or a service tool has no promise and no example, and the error names it.
- R14. R13 does not apply to private functions or to a program's `main`, and it does not change `mo check` or `mo run`.

**The change summary**

- R15. Given a previous version, `mo test` prints a change summary: the checks no longer true, the checks newly true, the checks about something new, the checks that could not be compared with the reason for each, and a count of the checks that pass on both versions.
- R16. Nothing in the change summary fails the run.
- R17. The change summary gives the digest of the previous version it compared against.

**The repository's own programs**

- R18. The library modules in `lib/`, the programs in `examples/` and the task queue pass both gates before the gates are on by default.
- R19. A fixture or golden case that exists to show something else keeps its program, and its recorded result is rewritten by hand to what the reference now requires.

**Documents**

- R20. Sections 12 and 13.4 of the reference describe the gates and the change summary, `docs/agent-guide.md` teaches them, and `docs/design/promise-strength.md` says that mutants are gated while wrong versions stay a measurement.

**The proof**

- R21. The predictions are written before the first run, and every result is kept, including a weak one.
- R22. The measured program is the task queue as frozen before its intent was strengthened. An agent that has seen neither the wrong versions nor anything written from them makes it pass the mutation gate, using only what the gate reports.
- R23. The measurement counts how many of the 24 wrong versions the fitted program kills, with any check able to kill, by kind of check and for each half of the set. A wrong version that the fitted program's own intent or code makes uncountable is reported separately and by name, and not as a survivor.
- R24. The proof also records the time of a full run for the task queue, the library and the examples, the time of a shorter run, the number of mutants nothing could kill, and the number of survivors that only a rare call sequence could kill.
- R25. No number decides whether the gate is kept.

**The review door**

- R26. The review door's check of a pushed program runs the checks it runs today under today's limits. It tries no mutants and does not apply R13 until the door has its own plan, so R7 and R8 do not reach it.

**Runs that are not the full run**

- R27. A run that selects some of a program's checks or modules tries no mutants and does not apply R13, and its result says the gates did not run.
- R28. The result of a shorter run says it is not the full run. The digest of R10 and R17 depends only on the previous version's source, so anyone can reproduce it from a checkout.

**The full run on the repository**

- R29. The nightly runs the full run on `lib/`, `examples/` and the task queue, and fails on a survivor.
- R30. The gates go on by default only after the measured full run of those programs fits the time the nightly and the local `drivers` group allow.

### Key Flows

- F1. An agent changes a program
  - **Trigger:** A2 has edited a Mo program and wants to know whether it is done.
  - **Actors:** A2, A3
  - **Steps:** A2 runs `mo test` and names the previous version. A3 tries the mutants of what changed and reports each survivor. A2 adds a check or simplifies the code, then runs again until nothing survives.
  - **Covered by:** R2, R3, R6, R9, R10, R13, R28

- F2. Robert reads a change
  - **Trigger:** A change is ready for A1.
  - **Actors:** A1, A3
  - **Steps:** The change handed to A1 names the commit its previous version came from, and A1 reads its change summary. Anything under "no longer true" or "could not be compared" gets his attention. A fix shows one or more checks under "newly true". A change with nothing in those three lists altered no checked behaviour.
  - **Covered by:** R15, R16, R17

- F3. The full run
  - **Trigger:** The nightly runs.
  - **Actors:** A3
  - **Steps:** `mo test` runs on each of the repository's programs with no previous version named and tries every mutant. This is the run every shorter run relies on.
  - **Covered by:** R8, R11, R29

- F4. The proof
  - **Trigger:** The mutation gate works on the task queue.
  - **Actors:** A1, A2, A3
  - **Steps:** The predictions are written. A2, kept from the wrong versions, fits the frozen task queue to the gate. A3 measures the 24 wrong versions against the result. The numbers of R24 are recorded and shown to A1.
  - **Covered by:** R21, R22, R23, R24, R25

### Acceptance Examples

- AE1. **Covers R2, R6.** Given a function that compares with `<`, and no check that tells `<` from `<=`, when `mo test` runs, then it fails and names the function, the line and that change.
- AE2. **Covers R3.** Given a mutant that only a test kills, when `mo test` runs, then the mutant counts as killed and the result says a test killed it.
- AE3. **Covers R5.** Given a function that no check runs, when `mo test` runs, then every mutant of that function survives and the run fails.
- AE4. **Covers R9, R10, R28.** Given a previous version in which one function differs from the new one, when `mo test` names that version, then only that function's mutants are tried, and the result gives the number of functions carried over and the previous version's digest and says it is not the full run.
- AE5. **Covers R9.** Given a function that did not change and a test of it that was removed, when `mo test` names the previous version, then that function's mutants are tried again.
- AE6. **Covers R13, R14.** Given an exposed function with a test and no promise or example, when `mo test` runs, then it fails and names the function. Given a private function with neither, then R13 reports nothing for it.
- AE7. **Covers R15.** Given a fix whose new check fails on the previous version and passes on the new one, when `mo test` names the previous version, then that check is listed as newly true.
- AE8. **Covers R15, R16.** Given a check of the previous version that the new version fails, when `mo test` names the previous version, then that check is listed as no longer true, and the run does not fail on that account.
- AE9. **Covers R15, R16.** Given a refactor that renames a private function, and a test of the previous version that names it, when `mo test` names the previous version, then that test is listed as could not be compared, and the run does not fail on that account.
- AE10. **Covers R11.** Given the same program and the same previous version, when `mo test` runs once with a cache and once without, then both runs give the same result.
- AE11. **Covers R27.** Given a program with a surviving mutant, when `mo test` runs with one check selected, then no mutant is tried and the result says the gates did not run.

### Success Criteria

- The proof's record states its predictions, the count of wrong versions killed and the numbers of R24, and Robert has seen it.
- Phase 7's M3 starts with the gates on, and its client library never has a version that did not meet them.

### Scope Boundaries

Deferred for later:

- The gate at the review door, which gets its own plan.
- Exceptions for mutants that cannot be killed.
- A complexity limit and a duplication check.
- Trying every call sequence for a service.
- A review door for programs that are not services.
- A form for stating effects as intent.

Outside this work:

- Locking tests so that a change to one waits for Robert.
- Changes to how wrong versions and `mo test --strength` work, beyond what R23 needs.
- Process ideas that sit outside the language, such as a second agent judging the first one's work.

### Dependencies / Assumptions

- A passing gate shows that a program's code is pinned by its checks. It does not show that the checks say what Robert wants. R13 and the change summary narrow that gap and do not close it.
- Phase 7's plan does not yet say that M3 waits for this work. That plan is on `main` and another session is working from it.
- The repository holds what was written from the wrong versions: the working half and its mistakes in `learnability/phase6/strength/`, each survivor in `docs/status/phase6-run.md`, and the intent strengthened against them in `services/task-queue/`. R22 therefore needs the fitting agent to work in a folder that holds only the frozen task queue, a built `mo` and Mo's own guidance, and its transcript is kept. The held-back half is kept outside the repository, as `learnability/phase6/strength/README.md` records.
- Unverified: the frozen task queue still compiles with today's `mo`.
- Unverified: what the gate costs. One pass of the task queue's checks takes about 68 seconds on a debug build, and a crude count gives about 650 places to change, so a run with no savings is on the order of ten hours. Stopping at the first kill and running only the checks that reach a change should cut that by a large factor that nobody has measured. The nightly has 120 minutes for all its groups. If the measured full run of R24 does not meet R30, the work stops and Robert decides.
- Unverified: mutants that nothing can kill are rare enough for R7 to hold. If the repository's programs meet one that neither a new check, simpler code nor dropping that kind of change resolves, the work stops and Robert decides.

- A limit of the change summary: a change that renames what a previous test names and also changes behaviour shows that test as could not be compared and shows nothing as no longer true. The summary cannot tell this from a rename alone.
- A limit of the proof: the wrong versions replace nothing that an intent item of the frozen task queue reads, so none touches `submit`, `lease`, `acknowledge` or `fail`. R23 says nothing about how the gate covers those operations, and the proof's record says so.

### Outstanding Questions

Resolve Before Planning: none.

Deferred to Planning:

- Which kinds of change the toolchain makes to produce a mutant.
- How a mutant that makes a check run without finishing is counted.
- How a shorter run decides that a function or its checks changed. The digest of an intent item in `docs/design/intent.md`, which covers the declarations the item reads, is the nearest existing rule.
- How a previous check that no longer compiles is reported one check at a time, and not as a whole file.
- The error codes and the wording of each report.
- How `mo test --strength` counts a kill by a test for R23 while its existing rate stays readable.
- In what order the gates are built, the repository's programs fitted and the gates turned on by default.
- How the 51 golden cases that run `mo test` change.
- Which file the task queue is gated and measured through. Its tests are in `queue-tests.mo`, which `main.mo` does not import, and `mo test --strength` runs `main.mo`.

### Sources / Research

- `GLOSSARY.md`: the vocabulary.
- `docs/design/promise-strength.md`: the wrong-version measure and its "no arbitrary pass threshold" rule. `src/tools/strength.c` replaces declarations, classifies outcomes and leaves tests out of a kill.
- `docs/design/intent.md`: intent items and their digests, the review door, what the door runs, its limits and the task queue's timings.
- `docs/status/phase6-run.md`: the 3 of 12 and 11 of 12 results and what the survivors were.
- `learnability/phase6/strength/`: the wrong versions, the manifest that hashes the frozen task queue, and the results.
- `docs/reference/README.md` sections 12 and 13.4: examples, promises, tests, fakes, recovery checks and the `mo` command.
- `ci/check_cli.py`, `ci/check_golden.py` and `ci/check_lib.py`: where the repository's checks run `mo test`.
- `AGENTS.md`, "Checks and merging": the treatment a claim about what Mo guarantees gets.
- Two outside projects were read for this work: Robert Martin's `ub-mode`, whose mutation, coverage and duplication tools fail a build, and Cursor's `pstack`, for the rules that a check which has never failed proves nothing and that the author of a change does not loosen its gate.
