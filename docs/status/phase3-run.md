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
| Drill unit, `halt` called until the supervisor stopped it | 15 `failure: halt` lines, `restart: 1 of 3` to `3 of 3`, then `mo serve: stop: service handler failed`. Exit 69, unit `failed`, `NRestarts=0` |
| Drill unit with no `QUEUE_TOKEN` | One start, `error E1409: mo serve could not read the token from environment variable QUEUE_TOKEN`, exit 1, unit `failed`, `NRestarts=0` |
| `systemctl stop task-queue` | `mo serve: stop: a signal asked the service to stop`, exit 0, `inactive`, no restart. `systemctl start` brought back job 1 and reset `NRestarts` to 0 |
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
