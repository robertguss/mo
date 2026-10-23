---
name: ship-task
description: Take a Mo task from Notion to an open pull request. Use when asked to start, build, do or ship an MO task, such as "do MO-89".
---

# Ship a task

One MO task is one PR off `main`. This skill is the order of the work; each step's own skill holds how to do it. Agents commit, push and open PRs freely; Robert approves every merge except docs-only PRs (AGENTS.md "Checks and merging").

## 1. Load the task

Find the task in Mo Tasks (the `notion-sync` skill names the IDs) and read its page, the tasks it is blocked by, and the docs it names. If a task it is blocked by is not Done, stop and tell Robert.

Done when you can state the task's deliverable in one sentence and nothing blocks it.

## 2. Branch from main

Fetch `main` fresh and branch from it as `mo-NN-<topic>`, unless your session assigns a branch name. Set the task's Status to "In progress".

## 3. Plan

Run `plan-change`. A behaviour a Mo user would notice and the task leaves open is Robert's call: post the options with your recommendation and wait for his answer before building.

Done when the plan has no open questions.

## 4. Build

Write the code and its tests following `test-change`, in commits that each leave the tree building.

Done when every behaviour in the plan has a test seen red, then green, and the `ci/check.sh` groups the change touches pass, or each skipped check is named with the tool it lacks. A PR merges when Robert says so (AGENTS.md "Checks and merging"); no Mo check runs on GitHub.

## 5. Review

Run `review-change`. It reviews in a fresh context, so it judges the change cold. Fix each finding, or write down why it stands, then rerun the checks the fixes touched.

Done when every finding is fixed or answered and the checks are green again.

## 6. Open the PR

Title it `MO-NN: <what it adds>` so the GitHub sync links it to the task, and fill in every section of the PR template. Then run `notion-sync` so the task shows as "In review".

Done when the PR is open and its body reports the local `bash ci/check.sh gcc` result on its final commit, plus any other group run and the groups not run.
