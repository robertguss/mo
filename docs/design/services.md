# Services: the Phase 3 design

Status: Design note (18 Oct 2026, task `MO-98`), updated for D301 to D308 in `MO-153`
and the D310, D312 and D313 clarifications in `MO-155`.
MO-155 adds service syntax and checking, with reference section 11.5. The service runtime
is not implemented yet. The final diagnostic and reference wording remains for Robert's
review before MO-155 merges. The pure queue example implements D307. Phase 2 is complete
(D300), so Phase 3 can begin (D267). D256 to D267 were settled with Robert on 18 Oct 2026;
section 9 records the decisions
he made on 24 and 25 Oct 2026.

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
or `Result((State, R), E)`. A tool returning exactly `State` or `Result(State, E)` is refused;
every other result shape
is a read-only reply, including `List(State)`, `Option(State)` and records containing `State`
(D302, clarified by D310 and refined by D313).
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
task-queue service. MO-155 adds the service block; running it needs the later runtime tasks
(section 9).

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
  Ok((after, Unit))
end

## Marks the current lease as done.
fn acknowledge(queue: Queue, now: Time, id: Int, attempt: Int) : Result((Queue, Unit), QueueError)
  after = try JobQueue.acknowledge(queue, now, id, attempt)
  Ok((after, Unit))
end

## Returns a failed lease to the queue, or marks its final attempt dead.
fn fail(queue: Queue, now: Time, id: Int, attempt: Int) : Result((Queue, Unit), QueueError)
  after = try JobQueue.fail(queue, now, id, attempt)
  Ok((after, Unit))
end

fn expire(queue: Queue, now: Time) : Queue
  JobQueue.expire(queue, now)
end
```

Every handler is a plain function. The cost is some repetition, since `queue` and `now` appear
on every handler. A read-only tool such as `count` has the signature
`fn count(queue: Queue, now: Time) : Int` and returns no replacement state. A timer handler takes
`(State, Time)` and returns only `State`.

The service selects a local overload by its first parameter's exact state type. The selected
tool or timer signature must contain no unresolved generic parameters. Generic helpers remain
usable through concrete wrapper functions; Phase 3 adds no explicit generic-specialization
syntax at this boundary. Robert approved this D301 follow-up on 25 Oct in the
[approval conversation](https://ampcode.com/threads/T-01a1705d-1274-74f9-8fcf-0bf58e6f89b8).

The clauses have a strict order (D312): exactly one `state`, exactly one `tools`
naming at least one function, zero or more `every` clauses, and exactly one `token` last.
Empty and timer-only services are refused. No function name may appear twice anywhere
in the block. Intervals are duration literals with a whole-number value of at least 1.
The start value is used only when there is no saved state. State, handler inputs and
handler results contain neither capabilities nor function values, including nested values.
The environment name contains only uppercase ASCII letters, digits and underscores,
and cannot start with a digit.

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

Robert settled D301 to D308 on 24 Oct 2026 and clarified D302 in D310 on 25 Oct.
D312 records the later MO-155 approval on 25 Oct, without changing D309, D310 or D311.
D313 refines D310 and D312 without changing those decision records.
These decisions define the Phase 3 build plan.

1. **Service block (D301).** A module has at most one top-level `service ... end` block. It
   declares `state Type = start value`, `tools a, b, c`, `every <duration> <handler>`, and
   `token from env "NAME"`. Handlers remain ordinary functions. In the D301 follow-up approved
   on 25 Oct, Robert reserved only `service`; `state`, `tools`, `every`, `token`, `from` and
   `env` remain ordinary identifiers outside their clause positions.
2. **Read-only handlers (D302, clarified by D310 and D313).** Only `(State, Reply)` and
   `Result((State, Reply), E)` update stored state. A tool returning exactly `State` or
   `Result(State, E)` is refused with E0101. Timer handlers use `(State, Time): State` to mean replacement
   state, so accepting a bare-state tool as read-only would look like an update that is never
   saved. Only a bare-State tool is eligible for two suggestions: return `(state, Unit)` to
   update, or wrap the value in a record to return it as read-only data. Functions on the
   module's `expose` line and names ending in `?` receive no fixes. `Result(State, E)` receives
   no fixes. `mo fix` applies neither alternative.
   Every other result shape is read-only, including
   `List(State)`, `Option(State)` and records containing `State`. The runtime neither logs nor
   saves these messages. The [D310 ruling](https://app.notion.com/p/404700320c45811da3a7deef66d2b37f)
   requires the diagnostic code and exact wording under "For Robert" in the MO-155 PR.
   [D313](https://app.notion.com/p/404700320c45815eb7d4fdd3858b31ee) refuses `Result(State, E)`
   because the queue's `heartbeat`, `acknowledge` and `fail` return `Result(Queue, QueueError)`.
   Direct wrappers must not silently treat accepted updates as read-only replies.
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
   once at startup. Simultaneous ticks follow the source order of the `every` clauses (D312).
   Timers are not saved state, and there are no one-off timers.
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

### MO-155 diagnostics and fixes, for Robert

[D312](https://app.notion.com/p/404700320c45814188abd07e6534971a) records the approved
rules from the [approval conversation](https://ampcode.com/threads/T-01a1705d-1274-74f9-8fcf-0bf58e6f89b8).
[D313](https://app.notion.com/p/404700320c45815eb7d4fdd3858b31ee) records the later refinement
from the latest approval relayed into the
[execution conversation](https://ampcode.com/threads/T-01a16ff1-e184-7194-808d-d6d0112ba4a4).
Both records paraphrase the approval rather than quote Robert's words.
All final diagnostic codes, diagnostic wording and reference wording still go under
"For Robert" in [PR #231](https://github.com/robertguss/mo/pull/231) before merge.
MO-155 remains In review. Final approval is still required before merge.

For a bare-State tool eligible for fixes, E0101 uses these strings:

- Headline: `a service tool returns only its state`
- Label: `a timer returns this; a tool returns (<State>, reply)`
- Reason: `A tool changes the state only by returning (state, reply). Returning the state alone is how a timer changes it, so a tool's result would never be saved (reference 11.5).`
- First fix title: `return (state, Unit) to save the new state`
- Second fix title: `return the state inside a record, as read-only data`

Each suggested adapter keeps the tool's name and `##` comment, renames the original
function to a helper, and rebinds module references to the helper. Functions on the module's
`expose` line receive no fixes. The state-changing alternative uses Mo's `Unit` value,
not `()` syntax.

Robert approved the no-fix exception for a handler whose name ends in `?` (D313).
Neither adapter can preserve that name while obeying the existing rule that `?`
functions return `Bool`. These cases keep the bare-State headline and label, with
the following reasons:

- `?` handler: `A tool changes the state only by returning (state, reply). Returning the state alone is how a timer changes it, so a tool's result would never be saved. A name ending in ? must return Bool, so rename the function before changing its result (reference 11.5).`
- Exposed handler: `A tool changes the state only by returning (state, reply). Returning the state alone is how a timer changes it, so a tool's result would never be saved. Other modules call this function, so change its result by hand (reference 11.5).`

A tool returning `Result(State, E)` receives E0101 with no fixes (D313):

- Headline: `a service tool returns only its state`
- Label: `a tool returns Result((<State>, reply), E)`
- Reason: `A tool changes the state only by returning (state, reply) or Result((state, reply), E). A Result holding the state alone is a read-only reply, so the new state would never be saved (reference 11.5).`

Generated adapters also
respect the existing type-depth limit; exhausting it publishes no partial choice.

E0100 uses `a service handler is generic` for a generic handler, with the reason
`Tools and timers have concrete signatures; call generic helpers through concrete wrappers (reference 11.5).`
The wrong-type reason is `The start value has the declared state type (reference 11.5).`
E0104 refuses capabilities and function values in state, handler inputs
or handler results. E1408 uses `this build cannot run a service` only for `mo run` of
a service entry module. Checking, testing ordinary handlers, formatting, fixing and
importing remain available. Importing a service module does not start it.
Emission retains the ordinary entry-point contract: `mo emit` needs `main` and emits
that program, not a running service. A service module without `main` receives E0079,
not E1408. `mo check --emit core` and `mo check --emit bytecode` remain available.

The queue uses its existing `Job.attempts` and `Leased(until)` fields for lease identity. The
shared `leased_job` check enforces the same rules for heartbeat, acknowledgement and failure.
An unknown id returns `NoSuchJob`; a non-current lease returns `NotLeased`. Heartbeat uses
checked time addition, like `lease`, and returns `BadLease(length: 30.minutes)` if the new expiry
would exceed the year 9999. It leaves every other job, the attempt count and `next_id` unchanged.

The exe.dev documentation describes persistent VMs that stop when their owner stops them and
does not describe idle sleeping. Deployment still requires a 48-hour idle check. That check and
the week-long run below have not happened.

### Fix follow-ups, not implemented in MO-155

Robert's PR #231 review records these later improvements:

- Emit smaller source edits instead of whole-file replacements.
- Format generated adapters and derive helper and record names from the tool's name.
- Retain E0101 alongside a resource-refusal diagnostic when generating fixes exceeds a limit.

These are design follow-ups, not claims about the current implementation. They add no service
runtime or deployment work to MO-155.

## 10. Done means

Mo's own agents take every task from the job queue for one week with no restart, and a deliberate
crash in the middle of that week loses no accepted job (D260). This replaces the earlier finish
line of 24 hours under synthetic load.
