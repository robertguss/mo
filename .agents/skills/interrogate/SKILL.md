---
name: interrogate
description: Adversarially review a Mo change with independent reviewers from different model families and synthesise one lead verdict without auto-applying fixes.
---

# Interrogate (Mo)

Adapted from pstack `interrogate` by poteto/Lauren Tan (MIT, via cursor/plugins) for Mo. The adversarial signal comes from model diversity, not assigned personas. Reserve for contested designs, security boundaries (capabilities, bytecode loading, heap reuse), and pre-merge review of risky diffs — it is expensive; routine diffs get a normal review.

## Workflow

1. **Scope.** From the user's pointer, or `git diff` against the base branch. Package the diff with the surrounding context reviewers need: public headers, the `.def` tables it touches, relevant tests, and the governing design doc.
2. **Intent.** State the intent in one paragraph from the request, commits, and code. If unsure, ask before spawning reviewers.
3. **Spawn reviewers in parallel**, one per available model family (Mo's learnability rounds use two families; independence is the point), each read-only, each given the same intent, diff, and rubric: correctness, capability-security (can this reach authority it wasn't handed?), memory safety under Tiger Style (unchecked returns, silent conversions, arena misuse), and contract fidelity (does it weaken a reference promise or a Lean-proved rule?). Reviewers never see each other's findings.
4. **Synthesise.** Merge findings that describe the same issue differently. Findings two or more reviewers raised independently are highest signal. Note explicit disagreements.
5. **Lead judgement.** You are the lead reviewer, not a neutral aggregator. Bucket every finding: **Act on** (blocks merge), **Consider**, **Noted**, **Dismissed** (with a one-line reason). Verify decisive claims against the code yourself before acting.

Do NOT auto-apply changes. 

**Output:** Intent, then the four buckets with reviewer attribution and evidence (file:line), then the verdict.
