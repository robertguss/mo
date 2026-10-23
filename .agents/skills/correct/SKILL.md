---
name: correct
description: Find the mistakes agents keep repeating in Mo and move each fix as high as it goes, from structure down to a check, a test, and prose last. Use for /correct, when Robert or a reviewer corrects the same kind of mistake twice, or when asked which agent mistakes recur.
---

# Correct repeated mistakes

Robert and the reviewers keep correcting agents for the same kinds of mistake. Change the repo so the next agent can't make them. Assume every contributor is an agent that opens a few files, copies the nearest example and takes the shortest path that builds; a change that looks right from one file has to be right for the whole repo.

The ledger is `docs/status/agent-mistakes.md`: one row per mistake class, with its evidence, the level that now enforces it, and its state. Read it first; a class already there that recurred is the top priority, because whatever enforces it didn't work.

## 1. Collect corrections

Read, newest first, since the ledger's "Last run" line (all history on the first run):

- merged PRs' reviews and review threads, and the "Review" section of each PR body, where `review-change` findings are recorded;
- commits inside each PR after its first: "fix review findings", "fix CI", "format", conflict merges;
- Robert's messages in the project threads that correct an agent, when the project's fetch tools are available;
- reverts and follow-up PRs that fix a just-merged one.

Write one record per correction: PR, source, what the agent did wrong in one sentence with the file, and whether an existing check caught it.

Done when every PR in range has been read and each correction has a record.

## 2. Group into classes

Group records by what went wrong, not where. A class counts once it has happened twice; keep one-offs in the run notes and drop them from the ledger.

Done when each class has a count, its PRs, and one short quote.

## 3. Pick the highest level that works

For each class, take the first level that can stop it, and write down why each level above it can't:

1. **Structure.** One source of truth instead of hand-synced lists (the way `prims.def` feeds four readers); one supported way to do a job; dead code an agent would copy deleted; a file layout where parallel PRs add separate files instead of editing the same lines.
2. **The C type system and compiler.** An enum that `-Wswitch-enum` makes exhaustive, `MO_MUST_USE`, a named limit in `src/base/limits.h`, an `MO_ASSERT` that fires in every test run.
3. **A check.** A `ci/check_*.py` script or `make` target wired into a `ci/check.sh` group, whose error names the file and the fix. If the bad pattern is already common, fail only when a change adds more.
4. **A test** that goes red when the behaviour breaks (see `test-change`).
5. **Prose, last**, only for judgment calls: a rule in `AGENTS.md`, `review-change/rules.md` or a skill. Nothing fails when an agent skips it.

A class whose cause is the cloud environment (a missing tool, a pinned version) goes to `.agents/setup` or a `ci/check.sh` message, not to prose.

Done when every class has a level and a reason the higher levels don't work.

## 4. Fix and prove

Fix the most frequent classes, one commit each, following `test-change`. Prove each new check against a real past mistake: re-apply the bad diff from the PR that made it (in a scratch branch), run the check, and see it go red with the message that names the fix. Then restore and see it green. A check never seen red on a real mistake proves nothing.

Done when each fixed class has a commit and a recorded red run on its past mistake, and the `ci/check.sh` group the check joined passes.

## 5. Update the ledger

Each class keeps one row: evidence (PRs), level, what enforces it (the file, check or rule), and state (`enforced`, `prose only`, `open`). Move a rule out of `AGENTS.md` or a skill once a check makes the mistake impossible; the prose is then noise. Update "Last run" to today's date and the newest PR read.

When Robert corrects an agent between runs, the agent that was corrected fixes the mistake and adds or bumps its ledger row in the same PR. A row that was already `prose only` and recurred is fixed at a higher level in that PR.

Done when every class has one row with its level, enforcement and state, and "Last run" names today and
the newest PR read.

## Report

Per class: its evidence, the level picked and why higher levels didn't work, and the commit or the reason it's left open. Then the classes left as one-offs.
