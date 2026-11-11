---
title: Keep freeze-hashed files unchanged after a dress rehearsal
date: 2026-11-11
category: workflow-issues
module: learnability/phase5/evaluation
problem_type: workflow_issue
component: testing_framework
severity: high
applies_when:
  - A dress rehearsal on one commit stands in for repeating live sessions on the frozen build
  - The freeze compares its hashed files between the rehearsed commit and merged main
  - Simplification, code review or record-keeping still run after the rehearsal
tags: [evaluation, freeze, rehearsal, evidence, code-review, simplification]
---

# Keep freeze-hashed files unchanged after a dress rehearsal

## Context

The Phase 5 close plan let one real-client development pair, run on the PR1 branch, stand in
for repeating those sessions on the frozen build. The condition: every file the freeze hashes
must be byte-identical between the rehearsed commit and merged main, or the rehearsal repeats.
Repeating costs real-client sessions, which count as development attempts.

`evidence.py freeze` hashes a lot: every `learnability/phase5/evaluation/*.py` (tests
included) and `learnability/phase4/*.py`, `learnability/phase5/evidence.py`,
`protocol.md`, `collection.md`, `results/<experiment>/declaration.json`, the prompt, the
fixtures, the build inputs, the drill and pause instruments and the binaries.

The ordinary shipping steps run after the rehearsal and edit exactly those files. In this
session three edits landed after the rehearsal:

- development-attempt counts added to the experiment's `declaration.json` (reverted before push);
- a simplification pass across several harness modules and tests (reverted);
- a real bug fix in `evidence.py`: a local argparse variable named `freeze` shadowed the new
  `freeze()` function, so `evidence.py freeze` crashed from its command line. This could not be
  reverted; Robert accepted that one difference instead of a repeat rehearsal, and the
  acceptance is recorded beside the rehearsal in `integration-development.md`.

## Guidance

- **Order the work:** run simplification and code review, and exercise every tool the freeze
  itself will run (such as `evidence.py freeze` from its command line), before the rehearsal.
- **After the rehearsal,** put records and evidence only in files the freeze does not hash. Here
  that was `learnability/phase5/evaluation/integration-development.md` (the harness group globs
  `*.py`, not `*.md`) and `learnability/phase5/test_evidence.py` (outside `learnability/phase5/evaluation`).
- **Defer** cosmetic and simplification findings on hashed files until collection ends; list
  them as unapplied in the PR instead.
- **If a hashed file must change,** get the owner's decision before the freeze, record it next to
  the rehearsal, and show the exact difference:

```sh
git diff --name-only <rehearsed-commit> <candidate> -- \
  learnability/phase5/evaluation learnability/phase4 learnability/phase5/evidence.py \
  learnability/phase5/protocol.md learnability/phase5/collection.md \
  learnability/phase5/results/<experiment>/declaration.json
```

## Why This Matters

The shipping workflow treats post-review edits as routine, so nothing in it warns that a freeze
depends on files staying still. Each routine edit to a hashed file silently turns a valid rehearsal
into a required repeat.

## When to Apply

- Any plan where a rehearsal or qualification run stands in for repeating it on the frozen build.
- Before applying review or simplification findings between that run and the freeze.

## Examples

Experiment 4's freeze record (`learnability/phase5/results/experiment-4/freeze/verification.json`,
`changed_since_rehearsal`) lists the four files that changed after the rehearsal; only
`evidence.py` was hashed, and it is the accepted exception.

## Related

- [Launch scored evaluation runs detached from the agent's tool session](launch-scored-runs-detached-from-the-agent-session.md)
- [Qualify a trace-parsing instrument against the real client first](../best-practices/qualify-trace-instruments-against-real-clients-first.md)
