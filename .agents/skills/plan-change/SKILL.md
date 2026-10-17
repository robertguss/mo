---
name: plan-change
description: Plan a Mo change before writing code. Use when starting an MO task, adding a library function or capability operation, or touching more than one module.
---

# Plan a change

A plan names every file the change touches and why, before any code exists. Its job is to make the change land in the right module, cross each shared contract on every side, and fit in one PR. The rules it plans against live in `AGENTS.md` (Architecture, C rules); this skill is the procedure.

## Steps

1. **State the behaviour.** One sentence on what a Mo user sees differently: what `mo check`, `mo run` or `mo test` now does. A change with no visible behaviour (a refactor, a CI fix) says what stays the same instead. If the task doesn't pin the behaviour down, ask Robert before planning further: he decides anything a Mo user would notice. Done when the sentence names a command and its new output or outcome.

2. **Find the contract.** Locate the section of `docs/reference/README.md` or `docs/design/` that defines the behaviour, and quote its heading in the plan. Where the code and the document disagree, the document wins; record the disagreement as an open question. Done when every behaviour in step 1 points at a section, or is marked as having none.

3. **Trace a precedent.** Find the most recent merged change of the same shape and list the files it touched: `git log --oneline -- <path>`, then `git show --stat <commit>`. For example, MO-83 (`List.all?`, commit 5375209) is a library-function precedent, and the last change to `src/base/hostops.def` is a capability-operation one. The precedent's file list is the plan's starting list. Done when the plan cites a commit, or says no precedent exists and why.

4. **Map the seams.** For each module in the pipeline, say whether it changes and why. Place new code in the module that owns the concept, and check each new `#include` against the module table. For each shared contract crossed (Mo Core, bytecode, `prims.def`, `hostops.def`), list every reader that changes in the same PR. Done when every file on the list is justified by a module or contract line, and every crossed contract lists all its readers.

5. **Choose the tests.** Use the `test-change` skill to pick the layer for each behaviour. Done when each behaviour from step 1 has a named test file or fixture.

6. **Size it.** One MO task is one PR. If the plan holds more than one change a reviewer would judge separately, split it into tasks and plan the first. Done when the plan is one PR, or the split is written down.

## The plan

Write it in this shape, in the PR description draft or the thread:

```
Behaviour:  <one sentence>
Contract:   <doc section(s)>
Precedent:  <commit, or none and why>
Files:      <path: why>, one per line
Contracts crossed: <contract: every reader updated>
Tests:      <behaviour: layer and file>
Open questions: <each one asked, or none>
```

Start building only when every open question is answered.
