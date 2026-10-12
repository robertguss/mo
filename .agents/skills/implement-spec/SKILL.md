---
name: implement-spec
description: Build an existing Mo spec to completion one reviewable, verifiable pass at a time, delegating independent slices in parallel.
---

# Implement Spec (Mo)

Adapted from dzhng/skills `implement-spec` (MIT) for Mo. Pairs with `write-spec` (the plan) and `close-spec` (the archive).

## Workflow

1. **Load the spec and prove the base.** Read the spec, the modules it touches, and `docs/design/implementation.md` (Tiger Style, the five conditions). Run `bash ci/check.sh` or the slice's gate first and record the baseline; separate pre-existing failures from yours.
2. **One slice, one pass.** Implement slices in dependency order. Each pass: implement, run the slice's named gate (`make test CC=clang`, `make sanitize`, `make grammar`, `make formal`, or the CLI check), and only then move on. Keep `MO_ASSERT` live, every return checked (`MO_MUST_USE`), arenas over scattered malloc/free, no silent numeric conversions.
3. **Delegate independent slices in parallel** only when they share no files or state; the coordinator integrates and re-runs the gate for the combined result. Never let two passes edit the same slice concurrently.
4. **Re-slice when the plan is stale.** If a slice hides multiple questions or an unproven seam, stop, update the spec via `write-spec`, and continue. Do not absorb deviations silently.
5. **Final gate.** `bash ci/check.sh` clean (or every skip/failure named), `make format` applied, diff reviewed via `interrogate` for contested or security-boundary changes.

**Done when:** every slice's gate passes on the combined tree, the spec and implementation agree, and remaining failures are reported as pre-existing with evidence.
