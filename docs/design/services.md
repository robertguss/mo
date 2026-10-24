# Services: the Phase 3 design

Status: Design note (18 Oct 2026, task `MO-98`). Nothing here is implemented, and none of it is
part of the Mo Language Reference v0.1 yet. Building starts once Phase 2 is done (D267). The
decisions below are D256 to D267, made with Robert in the roadmap brainstorm of 18 Oct 2026; the
open questions in section 9 are still to be settled with him.

## 1. Purpose

Mo's first niche is long-running services and agent infrastructure (D193). Phase 3 proves it by
running a service that Mo's own agents depend on every day: a job queue, written in Mo, that the
project's agents get their tasks from, over MCP (D256). The same service is the one the
north-star demo later diagnoses and upgrades live.

## 2. Scope

Phase 3 builds only what that job queue needs (D257):

- processes and messages, with the runtime owning each service's loop and mailbox;
- supervision;
- timers, for lease expiry;
- saved state, so no accepted job is ever lost;
- MCP tools served over the network.

These move to a later phase: a multi-core scheduler, an HTTP client, TLS, and database access for
Mo programs.

## 3. The demo: Mo's own task queue

Notion stays the plan, and the queue runs the work (D259). When a task is ready, the coordinator
submits it to the queue. A thread agent leases a task, checks in while it works, and marks it
done or failed. If an agent dies mid-task, its lease runs out and the task returns to the queue.
The queue never talks to Notion, so it needs no HTTP client or TLS.

The queue's logic already exists as a pure module, `examples/job-queue.mo`: `submit`, `lease`,
`acknowledge`, `fail` and `expire`, each taking a `Queue` and returning the new one (inside a
`Result` where the operation can fail). Its header promises that no operation ever loses a job,
and `lease` and `acknowledge` carry promises that say so. Phase 3 wraps that module as a service; it does not
rewrite it.

It runs on one of Robert's exe.dev VMs (D258). The VM's disk is persistent (local NVMe, streamed
to an off-machine backup), so the saved state survives restarts and updates, and the exe.dev
proxy terminates TLS, so Mo needs no TLS of its own.

## 4. The service model

A service is one state type plus pure handlers (D261). Each handler takes the current state and a
message and returns the new state and a reply. The runtime owns the loop, the mailbox and saving;
the service author writes none of them. This is the shape of Elixir's GenServer, with Mo's pure
values underneath.

Three things follow from it:

- Each handler's parameter and result types become its MCP tool's schema, and its `##` comment
  becomes the tool's description. An `Error(...)` result becomes a tool error.
- Phase 5's live-upgrade checker has exactly one state type to compare between versions.
- Examples, promises and tests call handlers directly, with no service running.

The runtime passes each message's arrival time to the handler, so handlers stay pure and need no
`Clock` capability.

## 5. Syntax

Handlers are ordinary functions, and one small `service` block in the module wires them up
(D266). The block names the state type and its start value, which functions are tools, the
timers, and where the access token comes from. This sketch turns `examples/job-queue.mo` into the
task-queue service; it is not valid Mo today, and its keywords are still open (section 9).

```mo
## Mo's own task queue. Agents lease a task, check in while working, and
## report it done or failed.
module TaskQueue
use JobQueue

service
  state Queue = JobQueue.empty
  tools submit, lease, acknowledge, fail
  every 1.minute expire
  token from env "QUEUE_TOKEN"
end

## Adds a task to the queue and returns its id.
fn submit(queue: Queue, now: Time, payload: Text) : (Queue, Int)
  JobQueue.submit(queue, payload)
end

## Leases the oldest ready task for 30 minutes, or returns None.
fn lease(queue: Queue, now: Time) : Result((Queue, Option(Job)), QueueError)
  JobQueue.lease(queue, now, 30.minutes)
end

## Marks a leased task as done.
fn acknowledge(queue: Queue, now: Time, id: Int) : Result((Queue, Unit), QueueError)
  after = try JobQueue.acknowledge(queue, id)
  Ok((after, ()))
end

fn expire(queue: Queue, now: Time) : Queue
  JobQueue.expire(queue, now)
end
```

Every handler is a plain function: state in, new state out. The cost is some repetition, since
`queue` and `now` appear on every handler. The sketch is not complete: it lists `fail` as a tool
without writing its wrapper, and it has no check-in tool yet (open question 5).

The rejected alternative put handlers inside the `service` block with the state implicit, so
assigning `queue` changed saved state. It read shorter, but `queue` and `now` appeared from
nowhere and saved state changed through an assignment the signature does not show, a hidden
effect in a language where everything else is explicit.

## 6. Saving

Every state change is saved before the reply goes out (D262). The runtime logs each message that
changes state and takes snapshots now and then. After a crash it loads the last snapshot and
replays the log. "Accepted" always means "on disk". The same log is what Phase 4 needs for exact
replay of failures.

The log and snapshots are stored in SQLite, embedded in the runtime (D263). SQLite is vendored as
C and pointed at Mo's own allocator, keeping the rule that only `base` and `heap` allocate memory
(D207). Service authors never see it, and Mo programs get no SQL access in Phase 3.

Saved bytes are untrusted. Loading a snapshot, replaying the log, and Phase 5 migrations must
validate each restored state value against the service's state type before any handler sees it.
Replay also validates each saved message against its handler's input type before calling it.
A migration reads state validated against the old type and must produce state valid for the new
type before the upgrade is accepted. Invalid tags, lengths, links or field values refuse the load
or upgrade with a diagnostic, never a crash or partially restored state. `heap` owns the bounded
decoder and value validation against runtime type descriptors; `host` owns reading snapshots and
log records from SQLite, and the runtime uses only validated values. This is not a `check` pass:
the VM never sees source. Implementation must add `fuzz/fuzz_saved_state.c`, with seeds under
`fuzz/corpus/fuzz_saved_state/`, covering snapshot and log decoding and rejection of malformed
values; Phase 5 extends it to migration input and output validation.

## 7. Access

Mo checks an access token itself (D264), so the service is not tied to exe.dev's proxy and the
same mechanism works for services that run elsewhere. The token reaches the service as a secret
value that never appears in printed output or logs.

## 8. Failures

A handler bug, such as a `stop` or a number overflow, fails only that message (D265). The caller
gets an error reply, the state stays exactly as it was before the message, and the service keeps
running. Nothing half-changed is ever saved, because handlers are pure and the runtime saves only
a handler's returned state.

A supervisor restarts the whole service only after repeated failures, for example five within a
minute. Unlike Erlang, Mo does not restart on every crash, since the last good state is always
there.

## 9. Open questions

Each of these is Robert's call, and each is settled one at a time and then recorded here.

1. **Keywords.** The exact words and layout of the `service` block (`service`, `state`, `tools`,
   `every`, `token from env`), whether a handler that changes no state still returns it, and
   whether `1.minute` joins the existing `minutes` for durations.
2. **Budgets.** What limits a service and each message gets (time, memory, mailbox length), what
   happens when one runs out, and where the limits are written.
3. **Timers.** How `every` behaves: whether a tick that is due while a message runs waits or is
   skipped, whether ticks missed during a restart are replayed, and whether one-off timers exist.
4. **Simulation mode.** How tests swap in pretend worlds for a service (D30): a fake clock that
   drives timers, simulated crashes and restarts, and messages fed without a network.
5. **Checking in.** `examples/job-queue.mo` has no operation for an agent to check in while it
   works, so a lease cannot be extended yet. The queue needs one, and its name and rules are open.
6. **The failure limit.** The exact count and window before the supervisor restarts a service;
   "five within a minute" is an example, not a decision.
7. **Idle VMs.** Whether an exe.dev VM sleeps when idle has not been checked, and a week with no
   restart depends on it.

## 10. Done means

Mo's own agents take every task from the job queue for one week with no restart, and a deliberate
crash in the middle of that week loses no accepted job (D260). This replaces the earlier finish
line of 24 hours under synthetic load.
