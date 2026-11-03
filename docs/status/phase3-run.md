# Phase 3 run

**Retired on 2026-11-02.** Phase 3 now closes on the recovery check in
`mo test` and the kill loop, both run on the development VM, and nothing
depended on the deployed queue. The 48-hour idle check that started at
2026-11-02 13:18 UTC was abandoned, not completed, and the week-long run never
started. Teardown below says how it was removed. The
rest of this file is the record as it was written, followed by the recovery
evidence that replaced the run.

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

## Teardown

On 2026-11-02, on `aurora-but-gold`: `claude mcp remove task-queue -s user`
removed the Claude Code entry, and `claude mcp list` no longer shows it;
`~/.config/task-queue/env.sh` was deleted, and the line in `~/.zshenv` that
sourced it was removed.

The queue's unit could not be stopped from `aurora-but-gold`: `ssh
turnpike-ferret.exe.xyz` answered "VM not found", and `ssh exe.dev ls` from
that key lists no VMs. The public route still answered an unauthenticated
request with 401.

On 2026-11-03 Robert deleted the `turnpike-ferret` VM, which took the unit, the
public route, the pinned build and the data folder with it. What remains is the
Codex `task-queue` entry and the token file on his Mac, which point at a host
that no longer exists.

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

## Recovery evidence

Results of the runs the predictions above were written for. Failures and
surviving mutants are kept.

### The recovery check in `mo test`

`build/gcc/debug/mo test services/task-queue/main.mo --only
Main.service.recovery` passes on the unmodified runtime within the default 2
seconds. The JSON form on the final build, run twice: `"sequences":179,
"restarts":572` both times, about 2.0 s each. Earlier runs ran 175 to 179
sequences and compared about 565 restarts, about 225 of them recovered after
passing the 100-commit snapshot boundary.

With the deadline lifted, 200 sequences reached the states that matter for the
queue: 37 steps held a Dead job, 38 re-leased a job after its lease expired, 10
acknowledged a job after a crash and restart, and no argument was refused at
copy-in. They compared 646 restarts, 252 of them past the snapshot boundary.

No service's check reported a mismatch or a refused recovery on the unmodified
runtime:

| Program | Sequences | Restarts compared |
| --- | --- | --- |
| `services/task-queue/main.mo` | 179 | 572 |
| `tests/lower-source/service-simulation/main.mo`, `Main` | 22 | 69 |
| `tests/lower-source/service-simulation/main.mo`, `Ticks` | 9 | 28 |
| `tests/service-simulation/task-queue.mo` (as `ci/check_simulation.py` lays it out) | 200 | 660 |
| `tests/golden/tst-simulation-admission/main.mo` | 200 | 657 |
| `tests/golden/service-read-only/main.mo` | 200 | 676 |
| `tests/recovery/two-services/main.mo`, `Main` and `Tally` | 200 and 200 | 635 and 730 |

`tests/golden/service-read-only` did change, as guessed. Its start value was
`stop "service initialization must not run during tests"`, which assumed
services never start under `mo test`. The decision: its start value is now an
ordinary empty list, and a new golden, `tests/golden/service-start-stops`,
pins the stopped report for a start value that stops.

### Negative control

`tests/test_recovery_faults.c` links a store loader that holds back the newest
log record of every load and hands it to recovery once, never or twice.

| Loader | Result |
| --- | --- |
| Unmodified (once) | Passes |
| Drops the newest record (never) | Fails the state comparison. Shrunk to `add(1)`, crash, restart; state 1 before the crash, 0 after |
| Repeats the newest record (twice) | Fails because recovery refuses the store: "saved service data is damaged or was written by a different program". Shrunk to `add`, crash, restart |

### Mutants of the checker, run by hand

| Mutant | Outcome |
| --- | --- |
| The oracle always reports equal | Killed |
| The shrunk sequence is not run again before it is reported | Killed |
| The final rerun ignores whether the failure was the same | Survived at first. Killed after adding `check_unrepeatable`, a fault that fires at the first restart only |
| The shrinker accepts a candidate without replaying it | Killed |
| Argument shrinking is skipped | Killed, only by the exact report text |
| Step deletion separates a crash from its restart | Killed, by an internal assertion |
| The shrinker accepts a failure of a different kind | Survived. Each fault loader yields only one kind of failure, so no test can tell |
| No cut after the first failing restart | Survived. The later deletion passes remove the trailing steps anyway |
| A single shrink round | Survived. One round already reaches the three steps here |

### Kill loop

`ci/check_kill_loop.py` sends seeded tool calls to the task queue under a real
`mo serve`, SIGKILLs it a random delay after a request is sent, restarts it on
the same data folder, and checks every job against a model built only from
replies. A kill counts as mid-commit when it leaves `service.sqlite-journal`
starting with SQLite's journal magic; the journal file itself always exists,
because SQLite runs in exclusive locking mode.

| Run | Kills | Mid-commit | Calls | Answered | Refused | Unknown | Result | Time |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Short, in `drivers`, seed 1 | 15 | 2 | 300 | 289 | 4 | 11 | Pass | 21 s |
| Long, `--seed 20261102 --kills 200` | 200 | 15 | 2443 | 2281 | 27 | 162 | Pass | 370 s |
| Long, `--seed 20261103 --kills 300` | 300 | 24 | 4492 | 4241 | 55 | 251 | Pass | 579 s |

The first long run used a build from before the recovery check landed; `mo
serve`'s code has not changed since. The second ran on the final build,
`10307199`, and printed:

```text
kill loop: seed=20261103 kills=300 mid_commit=24 folders=2 calls=4492 answered=4241 refused=55 unknown=251 result=pass seconds=579
```

No answered update was missing after a restart, and every update sent without a
reply was there once or not at all. The claim covers process death, not machine
or power loss: SIGKILL leaves the operating system's page cache intact, so a
write SQLite handed to the kernel survives it.

The poll loop spinning when file descriptors run out did not reproduce in any
kill-loop run: every run finished, none hung.

### Predictions against results

| Prediction | Result |
| --- | --- |
| The task queue's check passes in 2 s with a restart compared | Held |
| Dropping the newest record fails the comparison, shrunk to `add(1)`, crash, restart, 1 then 0 | Held |
| Repeating the newest record fails through the refusal, shrunk to the same three steps | Held |
| The kill loop loses no answered update and half-applies none | Held |
| The short kill loop finishes in under a minute | Held, 21 s |
| No fixture fails the comparison | Held |
| `service-read-only` reports stopped and needs a decision | Held; decided as above |
| 20 to 500 sequences, with snapshot crossings | Held: about 179, with about 225 restarts past the boundary |
| Byte equality raises no false alarm | Held: no mismatch in any run |
| Every long run of 100 kills or more lands some kills mid-commit | Held: 15 of 200 and 24 of 300 |
