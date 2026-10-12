---
name: close-spec
description: Archive a shipped Mo spec by rewriting it from a build plan into a durable rationale record that points at the code and decisions.
---

# Close Spec (Mo)

Adapted from dzhng/skills `close-spec` (MIT), folding in `handoff-spec`, for Mo's decision log.

## On shipping a spec

1. Rewrite the spec from future-tense plan into past-tense rationale: what was built, why this shape, which alternatives were cut and why. Delete step lists, checklists, and scaffolding prose that only made sense mid-build.
2. Point at the code, don't mirror it: name modules (`src/<module>/`), headers, tests (`tests/test_*.c`), and the gate that proves each claim (`make formal`, `make grammar`, `bash ci/check.sh`). Docs that duplicate code rot; pointers survive refactors.
3. Record decisions in Mo's numbered-decision style (D2xx) in the reference/design doc where the repo keeps them, including rejected options and the evidence that rejected them.
4. Note follow-ups explicitly as new questions, not as unfinished steps in the archived record.

## On handing a spec off mid-implementation

Capture in the spec itself: current slice, gate results so far (with commands), decisions made, open hypotheses, and the exact next verifiable step — so another agent or machine resumes without this session's context. Push the spec with the code state it describes.

**Done when:** a fresh agent can answer "why is it built this way and what proves it" from the archived record alone.
