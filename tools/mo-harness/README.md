# mo-harness: Confined Autonomous Coding Agent in Mo

`mo-harness` is a self-contained coding-agent runtime written in Mo that exercises the full
capability and actor stack ([why-mo.md](../../docs/why-mo.md)):

- **Workspace confinement by construction ([main.mo](main.mo)):** `confine_workspace` narrows the
  host `Files`, `Command`, and `Network` capabilities to a single target directory
  (`files.within(workspace)`), a static two-binary allowlist
  (`command.within(workspace).allow_programs(["mo", "git"])` backed by Landlock + seccomp on Linux
  via [lib/command.mo](../../lib/command.mo)), and the configured LLM gateway host
  (`network.allow_hosts([gateway_host_port])`).
- **Actor-supervised budgets & subagents ([coordinator.mo](coordinator.mo)):** `TaskCoordinator` is
  an in-process BEAM `actor` tracking turn counts, exact 128-bit `Decimal` USD token spend, and
  read-only scout subagent delegations (`files.read_only`), with machine-checked `promise`
  invariants (`after.steps >= before.steps`, `after.spent_usd <= after.budget_usd`) verified across
  simulated crashes and restarts by `mo test`.
- **Replayable SQLite trajectories:** Every model thought, typed `AgentAction` JSON payload, tool
  observation, and cumulative USD cost is persisted to `trajectories` and `turns` tables in
  `Sqlite.Database`.

## Commands

```sh
mo test tools/mo-harness/main.mo
mo run tools/mo-harness/main.mo run <workspace> <db> <run-id> <goal>
mo run tools/mo-harness/main.mo inspect <db> <run-id>
```
