---
name: review-change
description: Review a finished Mo change. Use after implementing, before opening a PR or marking it ready, or when asked to review a branch or PR.
---

# Review a change

The author is the worst reviewer of their own change, so the review runs in a fresh subagent that never saw the work, and the author verifies what it finds. CI already proves formatting, warnings, module includes and the library map; the review spends its attention on what CI can't see.

## Steps

1. **Gather the change.** Collect the diff against `origin/main`, the MO task or request it answers, its plan if one was written, and the latest `ci/check.sh` or CI result. Done when you have all four, or have noted which one doesn't exist.

2. **Dispatch the reviewer.** Start one subagent with the diff, the task, and this instruction: read `.agents/skills/review-change/rules.md` and `CLAUDE.md`, apply every rule to every changed file, and report each finding as `file:line`, the rule, and a failure scenario (a concrete input or state, and the wrong result it produces). Done when the reviewer returns with every rule marked applied.

3. **Verify each finding.** Reproduce it: run the failing input, build the case, or trace the path through the code to a real caller. A finding is confirmed when you have seen it happen or traced it end to end, and dropped otherwise. Done when every finding is confirmed or dropped, each with the evidence.

4. **Fix or hand over.** Fix every confirmed finding in this PR's scope, then re-run the checks it touches. A confirmed finding outside the task's scope goes in the PR description as a follow-up. Done when no confirmed in-scope finding remains open.

5. **Report.** In the PR description or thread: what was fixed, what was dropped and why, follow-ups, and the check result for the final commit.
