---
name: review-change
description: Adversarial review of a finished Mo change. Use when asked to review a diff, branch or PR, or as the self-review step before opening a PR.
context: fork
---

# Review a change

You run in a fresh context with no view of the conversation that built the change, so everything you judge comes from the repo. You are a **skeptic** reviewing someone else's change: assume each test is greener than it should be and each contract crossing is missing a side, until the code shows otherwise. If you can see the conversation that built the change, start a subagent with this skill instead of reviewing it yourself.

No Mo check runs on GitHub, so run the checks below rather than assume formatting, warnings, module includes and the library map are fine; then spend your attention on what no check can see.

## 1. Scope the change

Diff the branch against `origin/main` (for a PR, its base branch) and list every changed file. Find the MO task it answers (the PR title or branch names it) and the plan, if the PR description holds one. Read `AGENTS.md`, then `rules.md` in this skill's folder: they are the yardstick.

Done when you hold the full file list and the task.

## 2. Run the gates

Run `bash ci/check.sh gcc` plus each group the change reaches (AGENTS.md "Checks and merging" says which; a bare `bash ci/check.sh` runs every group, the slow ones included), and record each as green, red (with the first failing line), or skipped (with the missing tool). A red group is the top finding. A group you could not run is named as not run.

## 3. Apply every rule to every changed file

Work through `rules.md`. For the tests rule, plant a **mutant**: for each new behaviour, break it in the code (an off-by-one, a dropped check, a swapped branch), rerun its test, and confirm it goes red. Restore the code after each one. A mutant that stays green is a finding.

Done when every rule has been applied to every changed file, and each rule either produced findings or you can name what you checked to clear it.

## 4. Report

Findings ranked most severe first. Each one carries:

- `file:line`;
- the rule it breaks;
- a concrete failure: the input, mutant or caller that goes wrong, and how;
- the fix you would make.

Then one line per rule you cleared, naming the evidence, and the gate results from step 2. Keep what you verified apart from what you infer; a finding you could not reproduce says so.
