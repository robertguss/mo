---
title: Launch scored evaluation runs detached from the agent's tool session
date: 2026-11-11
category: workflow-issues
module: learnability/phase5/evaluation
problem_type: workflow_issue
component: testing_framework
severity: high
applies_when:
  - A run must not be interrupted once it starts, such as scored evaluation slots or a long drill
  - The run can outlast the agent tool's background-command limit
  - An interruption would be recorded as a result, not retried
tags: [evaluation, scored-session, harness, setsid, nohup, background, process-group]
---

# Launch scored evaluation runs detached from the agent's tool session

## Context

Phase 5 experiment 4 ran four scored slots with one loop script started through the agent
tool's background-command feature. That feature stops its command when its time limit (two
hours) runs out. Four slots can take four hours.

The collector, `learnability/phase5/evaluation/harness.py`, started in the tool's process
group. Its `launch()` puts the agent client in a new session (`start_new_session=True`), but
the harness itself stays in the group it was started from. The harness turns SIGTERM into a
clean exit (`stopped()` in `learnability/phase4/harness.py`). So a stop at the limit would have
ended whichever scored slot was running, and the protocol counts every launched slot with no
replacement: one tool timeout would have become a published miss.

By the time this was noticed, slot 1 had reserved, so relaunching would have spent it.

## Guidance

Start any run that must not be interrupted in its own session, with no terminal attached, and
watch it with a separate, read-only waiter:

```sh
setsid nohup ./run-slots.sh > console.txt 2>&1 < /dev/null &
```

The waiter, for example a loop that waits for `run finished` in `console.txt`, may be stopped
at any time without touching the run. `ci/check.sh` runs follow the same shape (AGENTS.md:
`nohup` plus `ci/wait-for-results`); scored runs need `setsid` as well, because the loop
script, not just its output, must leave the tool's process group.

If a run is already attached and a slot is live, do not restart it. What worked:

1. `kill -STOP` only the outer loop shell. Its running child (`/usr/bin/time` and the harness)
   keeps going; the stopped shell cannot start the next slot.
2. Start a `setsid nohup` runner that waits for the live harness PID to exit, kills the stopped
   loop (`kill -9` works on a stopped process), and runs the remaining slots with the identical
   command.
3. Record the hand-off in the experiment's README. Nothing in the collector, grader or freeze
   changes, so the slots stay comparable.

## Why This Matters

A scored slot is irreplaceable and its interruption is a result. An agent's background-command
limit is a property of the agent tool, not of the experiment, so it must never decide an outcome.
Checking the process group (`ps -eo pid,pgid,sid,args`) before launch shows whether the run would
share the tool's fate.

## When to Apply

- Before launching scored evaluation slots, kill drills, pause series or any run near or over
  the tool limit.
- When a waiter's timeout is shorter than the run it watches.

## Examples

Experiment 4's record ([results README](../../../learnability/phase5/results/experiment-4/README.md))
describes this hand-off: slot 1 ran to completion untouched; slots 2 to 4 ran from the detached
runner with the identical command.

## Related

- [Qualify a trace-parsing instrument against the real client first](../best-practices/qualify-trace-instruments-against-real-clients-first.md)
