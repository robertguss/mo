# Services: the Phase 3 design

Status: Design note (18 Oct 2026, task `MO-98`), updated for D301 to D308 in `MO-153`.
The service runtime and syntax are not implemented or part of the Mo Language Reference v0.1
yet. The pure queue example implements D307. Phase 2 is complete (D300), so Phase 3 can begin
(D267). D256 to D267 were settled with Robert on 18 Oct 2026; section 9 records the decisions
he made on 24 Oct 2026.

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
`heartbeat`, `acknowledge`, `fail` and `expire`, each taking a `Queue` and returning the new one (inside a
`Result` where the operation can fail). Its header promises that no operation ever loses a job,
and `lease` and `acknowledge` carry promises that say so. Phase 3 wraps that module as a service; it does not
rewrite it.

It runs on one of Robert's exe.dev VMs (D258). The VM's disk is persistent (local NVMe, streamed
to an off-machine backup), so the saved state survives restarts and updates, and the exe.dev
proxy terminates TLS, so Mo needs no TLS of its own.

## 4. The service model

A service is one state type plus pure handlers (D261). Tool handlers take the current state,
the message's arrival time, and the tool arguments. A state-changing handler returns `(State, R)`
or `Result((State, R), E)`; a read-only handler returns only its reply `R` (D302).
The runtime owns the loop, the mailbox and saving;
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
(D266, D301). The block names the state type and its start value, which functions are tools, the
timers, and where the access token comes from. This sketch turns `examples/job-queue.mo` into the
task-queue service. Its service syntax and singular duration unit are settled but not implemented
yet (section 9).

```mo
## Mo's own task queue. Agents lease a task, check in while working, and
## report it done or failed.
module TaskQueue
use JobQueue

service
  state Queue = JobQueue.empty
  tools submit, lease, heartbeat, acknowledge, fail
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

## Renews the current lease for 30 minutes from now.
fn heartbeat(queue: Queue, now: Time, id: Int, attempt: Int) : Result((Queue, Unit), QueueError)
  after = try JobQueue.heartbeat(queue, now, id, attempt)
  Ok((after, ()))
end

## Marks the current lease as done.
fn acknowledge(queue: Queue, now: Time, id: Int, attempt: Int) : Result((Queue, Unit), QueueError)
  after = try JobQueue.acknowledge(queue, now, id, attempt)
  Ok((after, ()))
end

## Returns a failed lease to the queue, or marks its final attempt dead.
fn fail(queue: Queue, now: Time, id: Int, attempt: Int) : Result((Queue, Unit), QueueError)
  after = try JobQueue.fail(queue, now, id, attempt)
  Ok((after, ()))
end

fn expire(queue: Queue, now: Time) : Queue
  JobQueue.expire(queue, now)
end
```

Every handler is a plain function. The cost is some repetition, since `queue` and `now` appear
on every handler. A read-only tool such as `count` has the signature
`fn count(queue: Queue, now: Time) : Int` and returns no replacement state. A timer handler takes
`(State, Time)` and returns only `State`.

The rejected alternative put handlers inside the `service` block with the state implicit, so
assigning `queue` changed saved state. It read shorter, but `queue` and `now` appeared from
nowhere and saved state changed through an assignment the signature does not show, a hidden
effect in a language where everything else is explicit.

## 6. Saving

Every state change is saved before the reply goes out (D262). The runtime logs each message that
changes state and takes snapshots now and then. After a crash it loads the last snapshot and
replays the log. "Accepted" always means "on disk". The same log is what Phase 4 needs for exact
replay of failures.

Read-only messages neither enter the log nor cause a snapshot (D302).

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

A supervisor restarts the service from its snapshot and log after 5 failed messages within
60 seconds (D308). After 3 restarts within 10 minutes, the service stops and stays down until a
person starts it. Every failure, restart and stop is logged with its reason. Mo does not restart
on every failed message, since the last good state is still available.

## 9. Settled

Robert settled D301 to D308 on 24 Oct 2026. These decisions define the Phase 3 build plan.

1. **Service block (D301).** A module has at most one top-level `service ... end` block. It
   declares `state Type = start value`, `tools a, b, c`, `every <duration> <handler>`, and
   `token from env "NAME"`. Handlers remain ordinary functions.
2. **Read-only handlers (D302).** A read-only handler returns only its reply. A state-changing
   handler returns `(State, reply)` or a `Result` wrapping that pair. The runtime neither logs
   nor saves read-only messages.
3. **Durations (D303).** All five existing units accept singular spellings: `1.second`,
   `1.millisecond`, `1.minute`, `1.hour`, and `1.day`. The syntax-only formatter normalizes
   only direct integer-literal receivers, singular for numeric value 1 and plural otherwise.
   It preserves grouped and arbitrary receivers, record fields and qualified function names
   (reference 3.9). Native signatures map each alias to the existing plural identity, so
   lowering, Core, bytecode and the VM need no new operations. Formatter recognition stays
   local to syntax; the independent test contract equates only literal-receiver aliases.
4. **Budgets (D304).** Per-message VM instructions, per-message memory and mailbox length have
   fixed runtime defaults, named in `src/base/limits.h`. A message over budget fails only that
   message. A full mailbox refuses new messages immediately with a busy tool error. Phase 3
   adds no limit syntax.
5. **Timers (D305).** A due tick is an ordinary mailbox message. At most one tick per timer is
   waiting at a time. Ticks missed while the service was down are not replayed; each timer runs
   once at startup. Timers are not saved state, and there are no one-off timers.
6. **Simulation (D306).** Examples, promises and tests can start an in-memory service, send
   tool messages, advance a fake clock to fire due ticks, and crash and restart the service.
   Crashes and restarts use the real log-and-snapshot code with a fake store. Simulation is
   deterministic and allowed only in tests, like fakes.
7. **Checking in (D307).** A lease is identified by `(id, attempt)`. `heartbeat(queue, now, id,
   attempt)` sets its expiry to `now + 30.minutes` without counting an attempt. It refuses an
   expired lease or a job that has moved on. `acknowledge` and `fail` also take `now`, `id` and
   `attempt`, and refuse a lease that is not current. Expiry includes `now == until`.
8. **Failure limit (D308).** 5 failed messages within 60 seconds restart the service from its
   snapshot and log. After 3 restarts within 10 minutes it stops until a person starts it.
   Every failure, restart and stop is logged with its reason.

The queue uses its existing `Job.attempts` and `Leased(until)` fields for lease identity. The
shared `leased_job` check enforces the same rules for heartbeat, acknowledgement and failure.
An unknown id returns `NoSuchJob`; a non-current lease returns `NotLeased`. Heartbeat uses
checked time addition, like `lease`, and returns `BadLease(length: 30.minutes)` if the new expiry
would exceed the year 9999. It leaves every other job, the attempt count and `next_id` unchanged.

The exe.dev documentation describes persistent VMs that stop when their owner stops them and
does not describe idle sleeping. Deployment still requires a 48-hour idle check. That check and
the week-long run below have not happened.

## 10. Done means

Mo's own agents take every task from the job queue for one week with no restart, and a deliberate
crash in the middle of that week loses no accepted job (D260). This replaces the earlier finish
line of 24 hours under synthetic load.
