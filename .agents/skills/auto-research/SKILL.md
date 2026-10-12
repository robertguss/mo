---
name: auto-research
description: Optimise Mo code, prompts or configs through fast progressive experiments, producing a verified candidate and a parameter-effect map.
---

# Auto Research (Mo)

Adapted from dzhng/skills `auto-research` (MIT) for Mo. Optimise learning per unit of time. This is the discipline behind `learnability/` rounds and fuzz/perf tuning.

## Workflow

1. **Recover state.** Read the objective, existing research records (`learnability/`, fuzz corpora), the incumbent best verified artifact, hypothesis queue, and remaining budget. Reuse record locations; collect a pending attempt before launching another.
2. **Clarify success before experimenting.** If evaluator, metric, baseline, or required improvement is unclear, ask and wait. State units exactly: "5% better" is ambiguous (percentage points vs relative). Passing correctness gates (`bash ci/check.sh`) and hitting the optimisation target are separate requirements. Record eval command, timeout, and promotion rule.
3. **Smallest useful loop.** Pick ONE cheap task that exposes the behaviour (one learnability prompt, one fuzz target for seconds, one bench input). Measure turnaround; strip setup from the inner loop. Do not build a benchmark matrix before the first trial.
4. **Baseline once.** Run the unchanged artifact, verify the evaluator detects a known-failing control, save exact inputs and raw output. Keep the baseline fixed per task/model/harness.
5. **Hypothesis batch.** From failure traces (not aggregate scores), list distinct hypotheses with: change + mechanism, predicted observation, falsifying result, cheapest discriminating test, cost. Rank by information per unit time.
6. **One hypothesis at a time** against the same parent, cheap checks first (`make test`, focused fuzz seconds) before the broad suite. Record keep/discard/inconclusive/invalid, regressions and nulls. Rejected candidates do not disprove the strategy.
7. **Combine winners, expand coverage.** Earlier tasks become regression checks; new failures become the focus. Before reporting any number: name the limiter, rule out measuring the wrong thing, repeat runs for noisy metrics.

**Deliverable:** best verified artifact plus the parameter-effect map (tested, helps/hurts, interactions). Missing results are unknown, never zero.
