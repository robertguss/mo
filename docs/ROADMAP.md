# Roadmap

Progress is tracked by phase, not by date. Each phase begins with a design
step for anything a Mo user would notice.

| Phase | Goal | Release |
| --- | --- | --- |
| 0. Reset | A clean foundation | |
| 1. Design the core | Decide what core Mo looks like and means | |
| 2. Agents can write it | A toolchain agents use every day | Internal milestone |
| 3. Services | Long-running services and MCP servers in Mo | |
| 4. Agents can see inside it | A runtime agents can operate | **v0.1**, the first public release |
| 5. It evolves while running | Typed live upgrades | v0.2 |
| 6. Intent and autonomy | People approve intent; agents ship the rest | v0.3 |
| 7. Fast | Fast enough for serious services | v0.4 |
| 8. Ecosystem | Others build and share | 1.0 |

Phases 0 to 2 are finished, and Phase 3 is the current phase.

## Now

Phase 3, Services. Eleven of its thirteen build tasks have merged: the
`service` block, its bytecode descriptor, the in-process runtime, SQLite saving
and recovery, simulation in tests, MCP over HTTP, `mo serve`, and the task-queue
service. The phase is done when the last item below passes. The PR that does an
item ticks it here.

- [x] The task-queue service: `services/task-queue/main.mo` from
      `docs/design/services.md` section 5, with simulation tests and one
      `drivers` check that runs it over loopback.
- [ ] Deployment on an exe.dev VM: a systemd unit that restarts after a crash
      but not after a deliberate stop (exit 69), a one-page runbook under
      `docs/ops/`, the proxy route, the MCP client line for agent threads, and
      a 48-hour idle check with no restart.
- [ ] The week-long run: the queue is the task list, every agent thread leases
      its task from it, at least 10 leases, a deliberate `kill -9` mid-week,
      and no accepted job lost. Recorded in `docs/status/phase3-run.md`.
- [ ] Only if it reproduces on the deployed queue: the poll loop spinning when
      file descriptors run out.

Waiting for Robert: nothing.

This file is the task list and the status. The designs are in `docs/design/`,
`docs/reference/` and `formal/`; the evidence is the tests, the checks and each
PR's description. Robert's Notion page for Mo mirrors this section.

## Later

Parked until a phase needs them: generated function-valued inputs in
`mo test`; simulation promise generators; supervisor event logging for the
protocol server; smaller `mo fix` edits for service adapters; the checker crash
on an interpolated `stop`; gcc sanitize, fuzz and a bounded arena proof in the
nightly; running a `.bc` file that `mo emit` wrote (Phase 5); `mo test
--strength` (Phase 6); Fil-C compatibility (Phase 7).

## Alongside every phase

Four tracks run alongside every phase:

- formal verification of Mo's core in Lean;
- evaluations of how well agents work with Mo;
- security;
- documentation.
