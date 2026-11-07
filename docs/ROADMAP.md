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

Phases 0 to 4 are finished. Phase 5, typed live upgrades, is next.

## Now

Phase 4, Agents can see inside it. Operator views, state export, failure capture and offline diagnosis are implemented. The diagnostic evaluation passed its declared gate: 14 of 16 correct, including two timing-defect misses. Robert approved v0.1.0 and the squash merge of PR #308 on 2026-11-07. Results and limits are in [the run record](status/phase4-run.md).

- [x] The design step: `docs/design/introspection.md` and the evaluation
      protocol `learnability/phase4/protocol.md`, with the experiments they
      rest on in `learnability/phase4/preflight/`. Approved on 2026-11-04.
- [x] Pilot campaign and diagnostic findings. The old independent-baseline prerequisite was superseded on 2026-11-05; no baseline was collected.
- [x] Operator tools on the MCP endpoint, and state export to a private file.
- [x] Failure records, kept apart from saved state.
- [x] `mo replay`, `mo query` and `mo history`, with the fix check.
- [x] A prospectively declared diagnostic evaluation: gate passed, all 16 sessions published with both misses. Comparative claims require a new independent baseline.
- [x] Robert’s v0.1 release decision and merge approval. The release tag must match the evaluation freeze’s build inputs.

Phase 3, Services, is done. It built the `service` block, its bytecode
descriptor, the in-process runtime, SQLite saving and recovery, simulation in
tests, MCP over HTTP, `mo serve`, the task-queue service, and the checks that
closed it. It closed on evidence, not on time: the results are in
`docs/status/phase3-run.md`.

- [x] The task-queue service: `services/task-queue/main.mo` from
      `docs/design/services.md` section 5, with simulation tests and one
      `drivers` check that runs it over loopback.
- [x] Crash recovery: every service's recovery check passes in `mo test`, the
      task queue's included; the negative control fails as designed; the kill
      loop passes in `drivers`, and a long run by hand is recorded.
- Removed: deployment on an exe.dev VM with a 48-hour idle check. Phase 3
  closes on the recovery check and the kill loop, so the deployment was
  retired.
- Removed: the week-long run. Phase 3 closes on evidence, not on time, and
  Robert starts every agent thread himself, so no thread leases from the queue.

Waiting for Robert: removing the Codex `task-queue` entry and the token file on
his Mac. The `turnpike-ferret` VM was deleted on 2026-11-03.

This file is the task list and the status. The designs are in `docs/design/`,
`docs/reference/` and `formal/`; the evidence is the tests, the checks and each
PR's description. Robert's Notion page for Mo mirrors this section.

## Later

Parked until a phase needs them: generated function-valued inputs in
`mo test`; supervisor event logging for the protocol server; the poll loop
spinning when file descriptors run out, which did not reproduce in the kill
loop; smaller `mo fix` edits for service adapters; the checker crash
on an interpolated `stop`; gcc sanitize, fuzz and a bounded arena proof in the
nightly; running a `.bc` file that `mo emit` wrote (Phase 5); `mo test
--strength` (Phase 6); Fil-C compatibility (Phase 7).

## Alongside every phase

Four tracks run alongside every phase:

- formal verification of Mo's core in Lean;
- evaluations of how well agents work with Mo;
- security;
- documentation.
