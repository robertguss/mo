---
name: correct
description: Find mistakes agents keep repeating in Mo and make each one impossible at the highest level that works, keeping a rule-to-enforcer table.
---

# Correct (Mo)

Adapted from pstack `correct` by poteto/Lauren Tan (MIT, via cursor/plugins) for Mo.

The operator keeps correcting agents for the same mistakes. Change the repo so the next agent can't make them. Assume every contributor is an agent that sees only the files it opened, copies the nearest example, and takes the shortest path that compiles. This is Mo's stated method in `docs/design/implementation.md` — the five conditions and `ci/check.sh` exist so checks, not intentions, enforce quality.

## Find the mistake classes

Read recent commits, reverts, review comments, and workaround comments. Group mistakes into classes. A class counts once it has happened twice.

## Fix each class at the highest level that works

1. **Architecture.** One owner per piece of state, one supported way per task, internals hidden so the wrong include fails (`make deps` enforces architecture section 2). Replace hand-synced lists with one source of truth (the `.def` tables: `src/base/prims.def`, `src/core/ops.def`, `src/bytecode/opcodes.def`); `make library-map` keeps docs in sync. Delete old ways an agent would copy.
2. **Types and compile-time.** Make the bad state unwritable; where C can't, add a check whose error names the fix: warnings-as-errors including `-Wconversion`, `MO_MUST_USE`, `MO_ASSERT` (never compiled out), `make tidy` (CERT ERR33-C).
3. **Test the behaviour.** Add or fix a `tests/test_*.c` check, fuzz target, grammar check, or Lean proof — whichever layer owns the contract (see `audit-tests`). Delete any test that still passes when every function it calls returns nothing.
4. **Docs or agent rules last**, only for judgement calls. Nothing fails when an agent skips them — that is why they are last.

## Fix and prove

Fix the most frequent classes now, one commit each. Prove each new check fails on a real past mistake (reintroduce it, watch the gate go red, revert). Run the same command locally and in CI (`bash ci/check.sh`). Exceptions go on the offending line with a reason and a human's approval.

## Keep the rule table

Maintain in the agent instruction surface a table pairing each rule with what enforces it (check name / make target). When corrected: fix the mistake, add the rule; if the rule existed but nothing enforced it, that is a repeat — fix it at the highest level in the same change. Drop a rule once its mistake can't happen.

**Reply:** each class with its evidence, the level picked, and why a higher level didn't work.
