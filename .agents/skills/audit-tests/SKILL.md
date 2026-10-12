---
name: audit-tests
description: Audit whether Mo tests earn their cost by mapping each contract to independent proof across C tests, fuzz targets, grammar checks and Lean proofs.
---

# Audit Tests (Mo)

Adapted from dzhng/skills `audit-tests` (MIT) for Mo.

Audit independent proof: what bug escapes if this test disappears? Fewer tests is not the goal; less maintenance for the same confidence is. In Mo the same contract may be claimed by a C test (`tests/test_*.c`), a fuzz target (`fuzz/fuzz_*.c`), the executable grammar (`grammar/`), and a Lean proof (`formal/`) — the audit decides which layer owns it.

## Workflow

1. **Bound the audit.** Pick one production responsibility (e.g. `src/heap/`, `src/core/text.c`). Record revision, local changes, and baseline `make test CC=gcc && make test CC=clang` results before editing. Separate pre-existing failures from unrun checks.
2. **Trace the claim.** Read each candidate test in full beside its public header and the code it reaches, plus sibling layers (fuzz corpus in `fuzz/corpus/`, grammar tests, Lean theorems). Write what the assertions actually detect, even when the name promises more.
3. **Record a disposition before editing** (Keep / Repair / Combine / Remove) with: location, detectable failure, why it exists, production consumers of any test-only hook, surviving owner, risk, validation command. An audit-only request ends here.
4. **Choose the surviving owner.** Prefer a real path with controlled edges over replaying internals. Another layer earns coverage only for a failure the owner cannot expose (e.g. bytecode loading, lifetime/reuse rules proved in Lean).
5. **Change one coherent boundary.** Carry unique cases into the survivor first (via the `write-tests` skill), then remove the old home and any test-only hooks in the same batch. Update Makefile/CI registration when moving suites.
6. **Prove preservation.** Diff removed vs surviving assertions, falsify transferred regression tests, run owner + sibling suites, then `make sanitize` and `bash ci/check.sh`. Report baseline failures separately.

## False confidence to hunt

- Circular evidence: expected output produced by the helper under test; a fixture copied from production tables (`src/base/prims.def`, `ops.def`).
- Exercise without a claim: calling every export to raise coverage while detecting no credible defect.
- The harness does the job: a stub supplies the ordering/acknowledgement production must generate.
- Count-only assertions on dedup/normalise paths.

Missing evidence stays an open question, not a deletion.
