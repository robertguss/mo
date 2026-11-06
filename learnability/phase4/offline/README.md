# Offline diagnosis evidence

This folder holds the evidence for Phase 4's offline commands: `mo replay` (with `--fix`),
`mo query`, `mo history`, and the `mo serve` store-lock wait that precedes them. The contract is
`docs/design/introspection.md` sections 4, 7, 8 and 9; the plan is
`docs/plans/2026-11-06-1220-feat-phase4-offline-diagnosis-plan.md`.

- `results/predictions.md` states what each check is expected to show. It was committed before any
  of those checks ran.
- `results/verification.md` records each check's command, result and evidence file, and keeps
  failed attempts beside the passes.
- `measure.py` measures the store reader behind `mo history` (its lock, memory and render time)
  and a `mo serve` start under a reader; `results/measurements.txt` holds its raw output.
