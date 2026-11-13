# Wrong versions of the task queue

Set 1, frozen on 2026-11-13 before the task queue's intent was strengthened. The measured
program is `services/task-queue/main.mo` and `job-queue.mo` as `manifest.json` hashes them.

- **Writer:** the session labelled `phase6-wrong-versions-1` (a Claude subagent, id
  `a979ac547a20f3a51`), started by the session that strengthens the intent and given only the
  program, the wrong-version format of `docs/design/promise-strength.md` section 15, and a
  check that hid every outcome except `invalid` and `changes_intent`.
- **Split:** 24 versions shuffled with `random.Random(61113)` and renamed `w01.mo` to `w24.mo`.
  `w01` to `w12` are the working half, in `working/`. `w13` to `w24` are the held-back half, kept
  outside the repository in `/home/exedev/code/mo-strength-held-back/set-1` with their mistakes,
  until their result is shown to Robert. The manifest carries every file's SHA-256 and the
  working half's mistakes only.
- **Measure:** `mo test services/task-queue/main.mo --strength learnability/phase6/strength/working`.
  Results are in `docs/status/phase6-run.md`.
