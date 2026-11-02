# Phase 3 run

The task queue's deployment and the runs recorded against it. Every result
below is an observed command outcome. `docs/ops/task-queue.md` holds the
commands.

## Deployment

Deployed 2026-11-02 by following `docs/ops/task-queue.md` on the queue VM.

| Setting | Value |
| --- | --- |
| VM | `turnpike-ferret`, a dedicated exe.dev VM (Ubuntu, systemd 255) |
| Route | `https://turnpike-ferret.exe.xyz/mcp`, public, proxy port 8000 |
| Pinned build | `main` at `c4fcd6c36b038034ee43ee6103cfd4b595c2f62b`, in `/opt/mo/c4fcd6c36b038034ee43ee6103cfd4b595c2f62b`, root-owned and read-only; a write to its `lib/` is refused |
| Unit | `task-queue.service`, enabled, user `taskqueue`, `--allow-host turnpike-ferret.exe.xyz` |
| Restart policy | `Restart=on-failure`, `RestartPreventExitStatus=1 2 69`, `RestartSec=5s`, 5 starts in 600 s |
| Data | `/var/lib/task-queue`, `taskqueue`, mode 0700 |
| Token | `/etc/task-queue/env`, root, mode 0600; on each client in `~/.config/task-queue/env.sh`, mode 0600 |
| Clients | Claude Code 2.1.309 on `aurora-but-gold` (user scope); Codex 0.164.0 on Robert's Mac |

## Drills

| Drill | Observed |
| --- | --- |
| SIGKILL to the main process | Active again 5 s later under a new PID (1822 → 1876), `NRestarts` 0 → 1. Drill job 1, submitted before the kill, still listed. A second kill with the runbook's fixed command: 1876 → 1901, `NRestarts` 1 → 2 |
| A copy of the unit whose `ExecStart` exits 70 (`task-queue-exit70`, removed after) | Restarted every 5 s: `NRestarts=5`, `ExecMainStatus=70`, then `failed` at the start limit. Added after review, because SIGKILL alone would also pass under `Restart=on-abnormal` |
| Drill unit, `halt` called until the supervisor stopped it | 15 `failure: halt` lines, `restart: 1 of 3` to `3 of 3`, then `mo serve: stop: service handler failed`. Exit 69, unit `failed`, `NRestarts=0` |
| Drill unit with no `QUEUE_TOKEN` | One start, `error E1409: mo serve could not read the token from environment variable QUEUE_TOKEN`, exit 1, unit `failed`, `NRestarts=0` |
| `systemctl stop task-queue` | `mo serve: stop: a signal asked the service to stop`, exit 0, `inactive`, no restart. `systemctl start` brought back job 1 and reset `NRestarts` to 0 |
| A copy of the unit pointed at a missing env file (`tq-envtest`, removed after) | `mo` never ran: `Result=resources`, `NRestarts=5`, then `failed` at the start limit. The real queue was untouched |
| Reboot | New boot `7162c7a9`. The queue started at boot, at 13:16:58 UTC, the same second as the boot's first journal entry, and listed job 1 |
| Health call through the public URL | `stats` answered |
| Wrong token through the public URL | 401 |
| Claude Code, runbook client line, through the public URL | `claude mcp get` shows `Authorization: Bearer ${QUEUE_TOKEN}` and `Connected`. A `claude -p` thread listed the seven tools and called `stats` |
| Codex, runbook client line, through the public URL | `config.toml` stores `bearer_token_env_var = "QUEUE_TOKEN"`. A `codex exec` thread listed the seven tools and called `stats` |
| Token anywhere it should not be | Not found in the VM's journal, unit files, shell histories, `/opt/mo` or the data folder; not in either client's config, shell history or Claude Code and Codex transcripts; not in the repo |

Drill job: id 1, payload "Restart drill: submitted before SIGKILL. Not a
task; acknowledge it.", leased and acknowledged at attempt 1. It is the only
job, and it is Done.

Fixes made while following the runbook: the drill program declared
`module Drill` in `main.mo` (E0035, now `module Main`), and `systemctl kill`
without `--kill-whom=main` printed an error about auxiliary processes. The
drill's `reset-failed` printed "not loaded" because the reboot came after the
exit-1 drill and unloaded the failed unit; in the runbook's order it clears the
failed state.

## Idle check

The 48-hour window started with `systemctl restart task-queue` at
2026-11-02 13:18:14 UTC: MainPID 447, `NRestarts=0`, boot `7162c7a9`.

First `stats`, 2026-11-02 13:18:17 UTC: total 1, ready 0, leased 0, done 1,
dead 0, limit 5000.

## Predictions for the recovery checks

Written on 2026-11-02, before the recovery check in `mo test` or the kill loop
existed, so before any run of either. Results go in their own section when the
checks run, and a prediction that turns out wrong stays here as written.

- The task queue's recovery check, `Main.service.recovery` on
  `services/task-queue/main.mo`, passes on the unmodified runtime on a gcc debug
  build within the default 2 seconds, with at least one restart compared.
- A runtime whose recovery drops the newest log record fails the comparison.
  Its counterexample shrinks to three steps: a tool call that commits an
  update, a crash, a restart. On the counter service that is `add(1)`, crash,
  restart, with state 1 before the crash and 0 after it.
- A runtime whose recovery repeats the newest log record fails because recovery
  refuses the store at the restart, not because the states differ. Its
  counterexample shrinks to the same three steps.
- The kill loop finds no update whose reply the driver received missing after a
  restart, and no update sent without a reply half-applied: each unanswered
  `submit` is there once or not at all.
- The short seeded kill loop in the `drivers` group finishes in under a minute.

Guesses, held less firmly:

- No service fixture in the repository fails the comparison: not
  `tests/lower-source/service-simulation`, whose `defect` and `busy` tools make
  the supervisor restart and stop it, not `tests/golden/tst-simulation-admission`,
  and not `tests/service-simulation/task-queue.mo`.
- `tests/golden/service-read-only`, whose start value is `stop`, reports its
  recovery check as stopped, so its expected output and status change. That
  golden says service initialization must not run during tests, so the change
  needs a decision, not only new expectations.
- The task queue's check runs between 20 and 500 sequences in 2 seconds on a
  gcc debug build, and at least one sequence crosses the 100-commit snapshot
  boundary through its one-minute timer.
- Byte equality raises no false alarm where Mo `==` would agree, because replay
  rebuilds Maps and Sets in the order the live run built them.
- The long kill loop lands some kills mid-commit, leaving
  `service.sqlite-journal` behind, in every run of 100 kills or more.
