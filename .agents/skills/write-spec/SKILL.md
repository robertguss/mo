---
name: write-spec
description: Break a large Mo feature into independently verifiable, human-reviewable slices at module seams, each ending in a check that runs in ci/check.sh.
---

# Write Spec (Mo)

Adapted from dzhng/skills `write-spec` (MIT) for Mo. A spec is a ladder of small contracts: each rung understandable to the human, testable by an agent, useful before the whole feature lands. Mo specs live as design docs (`docs/design/`) plus numbered decisions in the reference, not a separate specs/ tree — follow the repo's existing shape.

## First principles

1. **Grill before planning.** One question at a time, each with your recommended answer, until outcome, non-goals, sacred contracts, and first verifiable checkpoint are named. Inspect the repo instead of asking what code can answer. Default: no backward compatibility, no migration scaffolding — only the user opts in.
2. **Slice at module seams.** Each slice is a named boundary in `src/{base,bytecode,core,heap,tools}` (architecture section 2 owns allowed includes; `make deps` enforces it), with typed inputs/outputs, deterministic fixtures, and tests at the seam. If a slice needs three modules booted to check, sharpen the seam.
3. **Research the fog.** For unfamiliar domains, named references, or "how is this usually done," research primary sources first and record links in the spec. If a reference implementation exists, plan a replication spike before approximation.
4. **Every slice ends verifiable.** Name the exact gate per slice: a `tests/test_*.c` program, `make grammar`, `make formal`, a fuzz target, or a `mo` CLI invocation with expected output. Tests prove contracts; a runnable checkpoint exposes intent.
5. **Recursively re-slice.** After drafting, treat each high-risk slice as its own feature. Repeat until every next slice has one question, one seam, one review surface, one verdict.

## Workflow

Interview, research, then draft 2-3 differently-biased slice plans in parallel (different lenses; different model families if available) and synthesise. Agreement between blind drafts marks a solid cut. Write the spec with per-slice: goal, seam, contract, gate command, non-goals. Hand to `implement-spec`. Update the spec mid-implementation whenever the work proves it stale — it is a living document, and `close-spec` turns it into the durable rationale record.
