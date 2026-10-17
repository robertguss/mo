# Review rules

Apply every rule to every changed file. A finding needs a failure scenario: the concrete input or state, and the wrong result. Where you can't write one, the finding is a nit; label it so.

## Behaviour

- The change does what its task says, and what the reference or design doc says. Where code and doc disagree, the doc wins unless the PR changes the doc too.
- Any behaviour a Mo user would notice (syntax, diagnostics, CLI output, library names) traces to a decision by Robert, cited as `D2xx` or in the task.
- Expected values in tests and goldens come from the contract. An expectation edited to match new output, with no contract change named, is a finding.

## Contracts

- Each shared contract the diff crosses (Mo Core, bytecode, `prims.def`, `hostops.def`) is updated on every side that reads it, in this PR. `CLAUDE.md` lists the sides.
- Docs that describe the changed behaviour move with it: `docs/design/library.md`, `docs/status/sample-blockers.md`, the reference, the architecture section.

## Placement

- New code sits in the module that owns its concept, next to the code it most resembles. Code that passes `make deps` can still be in the wrong module.
- New names, file layout and helpers match the neighbouring code. An existing helper that already does the job is used rather than a new one.

## C rules

- Every rule in the "C rules" section of `CLAUDE.md`, applied to each changed function. The ones CI can't check carry the weight here: an `MO_ASSERT` on each invariant the code relies on, a named limit for each new size, depth or count, and arena allocation.

## Tests

- Each new behaviour has a test at the layer the `test-change` skill's table gives it, asserting what its "What a test asserts" section lists.
- Every existing assertion survives at full strength: none removed, loosened or skipped.

## Scope

- The diff is one MO task. Unrelated edits, drive-by refactors and reformatting of untouched code are findings.
- The PR title starts with the task, `MO-NN: ...`.
