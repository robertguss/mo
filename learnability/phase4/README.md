# Phase 4 evaluation

The evaluation of Phase 4, "agents can see inside it", follows `protocol.md`: fresh agent
sessions diagnose and fix seeded task-queue bugs, first with today's tools (the baseline),
then with the Phase 4 instruments. The rounds are compared with `compare.py`.

## The scenarios are private until the final round

A session can read this repository over the network. The scenario variants, whose
difference from the served queue is the bug, would give it away, so they live in a private
repository, `robertguss/mo-phase4-private`, checked out at mode 0700 beside this one. It
holds, under `variants/<name>/`, each variant's session files (`main.mo`, `job-queue.mo`,
`queue-tests.mo`), its bug report (`report.md`), its fixture (`fixture.json` and the rendered
`fixture-steps.mo`), its hidden tests with a reference fix and a broken fix, or its answer
key's rule, and `SHA256SUMS`; and `variants/README.md`, which says exactly what each variant
does. This repository holds the tools that read it, which take the variants' root as an
argument, and the plan (`docs/plans/2026-11-04-0315-feat-phase4-pilots-baseline-plan.md`),
which names each variant's family and shape and nothing more. The freeze will record every
private file by hash, and after the final round the private repository's history is
published and checked against it.

## The tools

- `fixture.py` builds a variant's data folder by replaying its step list into `mo serve`,
  renders the same step list into the module hidden tests replay into a simulation, runs
  the pre-session check against a running service, and computes a question's answer key.
- `grade.py` grades a session's folder: the variant's own tests over the session's, the
  hidden tests for a bug, the answer for a question. `grade.py --check` proves every variant
  sound: its reference fix passes, the variant and a broken fix each fail a hidden test,
  its report names no internals, and its hashes match.
- `testdata/variants/synthetic/` is a small queue with the served queue's tools and one
  deliberate toy defect, unrelated to any scenario. `test_fixture.py` and `test_grade.py`
  run on it, and `ci/check.sh bench` runs them; the six real variants are checked by hand
  (`grade.py --check MO PRIVATE_ROOT BUILT`), since CI cannot read the private repository.
- `preflight/` holds the experiments that sized the design; its README says what each found.
