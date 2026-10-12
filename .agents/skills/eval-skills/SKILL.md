---
name: eval-skills
description: Eval a Mo agent skill against golden cases with blind runs in fresh subagents and a separate judge, letting the gaps drive the edits.
---

# Eval Skills (Mo)

Adapted from dzhng/skills `eval-skills` (MIT) for Mo. Treat a skill like a function under test: feed example inputs in a clean room, grade the artifact, let failures drive edits. The eval is honest only if the run is blind — the executing agent carries none of this conversation's context and never sees the expected output.

## Inputs (refuse without them)

- **Target skill** — a real `.agents/skills/<name>/SKILL.md`. If it can't be found, list what exists and ask.
- **At least one golden case** — a concrete input the skill will actually receive (a prompt, a diff, a `.mo` program, a failing test). "Improve write-tests" with no input is not a case.
- **The bar per case** — the outcome a good artifact achieves and the smells that make it bad. This is a judgement bar ("sliced so each piece is independently buildable and verifiable; a lazy mega-slice and pointless over-splitting both fail"), not a prescribed parts list, unless the skill is genuinely conformance-style. Draw a fuzzy wish into a stated bar and echo it back before spending agents.

## Workflow

1. **Validate.** Read the target skill's first principles — the standard the judge grades against. Settle judgement vs conformance mode; ask if ambiguous.
2. **Blind run, one fresh agent per case.** Isolate each run (sandbox dir as the only writable root, not the live checkout). Give the runner only the input and the instruction to use the target skill — never the bar, other cases, or why you ask. Afterwards sweep the live checkout (`git status`) and clean leaks; isolation is best-effort, the sweep is the guarantee.
3. **Grade with a separate judge** given the artifact, the bar, and the skill's first principles — never the expected output, never "make this pass." Every part of the bar gets a verdict citing specific evidence.
4. **Edit from gaps, re-eval.** Fix the skill per `write-skills`, then re-run the failing cases blind. Report passes, remaining gaps, and cases never run.

Mo golden cases are cheap to source: past `learnability/` prompts and gradings, real diffs, and recorded CI failures.
