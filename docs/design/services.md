# Services: the Phase 3 design

Status: Design note (18 Oct 2026, task `MO-98`), updated for D301 to D308 in `MO-153`
and the D310, D312 and D313 clarifications in `MO-155`. D256 to D267 were settled with Robert
on 18 Oct 2026; section 9 records the decisions he made on 24 and 25 Oct 2026. This note
describes the design. Which parts are built is tracked in Notion's
[Status](https://www.notion.so/3fd700320c45810ca58ddf73fb6fc8c2) page and
[Mo Tasks](https://www.notion.so/657ad0b75b55427aa8310151d0468728), not here.

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
task-queue service. MO-155 adds the service block. The C service loop and SQLite
store have landed; `mo run` still does not start a service (section 9).

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
the VM never sees source. Snapshot and log decoding and rejection of malformed
values are covered by `fuzz/fuzz_saved_state.c`, `fuzz/fuzz_service_log.c` and
`fuzz/fuzz_service_snapshot.c`; Phase 5 extends them to migration input and output
validation.

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

[D308](https://app.notion.com/p/403700320c458174865ee7815f59aa4f) defines a failed message as
"a handler that crashed or went over budget". A normally returned `Error(...)` is an
application rejection, not a supervisor failure. It leaves the state unchanged without
advancing the failure count.

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

### MO-155 diagnostics and fixes

[D312](https://app.notion.com/p/404700320c45814188abd07e6534971a) records the approved
rules from the [approval conversation](https://ampcode.com/threads/T-01a1705d-1274-74f9-8fcf-0bf58e6f89b8).
[D313](https://app.notion.com/p/404700320c45815eb7d4fdd3858b31ee) records the later refinement
from the latest approval relayed into the
[execution conversation](https://ampcode.com/threads/T-01a16ff1-e184-7194-808d-d6d0112ba4a4).
Both records paraphrase the approval rather than quote Robert's words.

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
or handler results. E1408 uses `mo run does not start services yet` only for `mo run` of
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
does not describe idle sleeping. Deployment requires a 48-hour idle check (MO-163), separate
from the week-long run below (MO-164); unit tests satisfy neither.

### Fix follow-ups, not implemented in MO-155

Robert's PR #231 review records these later improvements, tracked as MO-171:

- Emit smaller source edits instead of whole-file replacements.
- Format generated adapters and derive helper and record names from the tool's name.
- Retain E0101 alongside a resource-refusal diagnostic when generating fixes exceeds a limit.

These are design follow-ups, not claims about the current implementation. They add no service
runtime or deployment work to MO-155.

### MO-156 descriptor and lowering

Service declarations compile to the optional bytecode descriptor in
`bytecode.md` section 3.6. Lowering carries `base/service.h` metadata alongside
Core: the start function, selected concrete tool and timer functions, tool
names and read-only flags, interval functions, and token variable name.
Codegen derives schema type indices from the compiled handler signatures.
The descriptor stores the complete result type; a later schema generator can
remove the state/reply and Result wrappers according to D313.

Mo Core and Lean need no new forms. The initializer and each interval are
ordinary zero-argument functions, using existing expressions, calls and
duration primitives. They are not global initializers. Core checks and text
round-trips these functions; Lean can evaluate their bodies without knowing
about service metadata. The Core wire format, evaluator, and VM instruction
set are unchanged. Core text alone does not carry service registration; the
source-to-bytecode compiler carries that separate metadata.

Intervals remain functions rather than a second compile-time duration
evaluator. This preserves checked arithmetic, including overflow, and keeps
the source-order timer table separate from function-table order. MO-156 adds
no runtime, environment reads, mailbox, persistence or network listener.

### MO-157 in-process loop

`vm/service.c` owns one service's state, FIFO mailbox, timers and supervisor. Its C
embedding API is in `vm/vm.h`; there is no new module, bytecode form, primitive,
capability operation, Mo API or CLI command. Core and Lean still evaluate ordinary
functions. The embedding supplies a clock, a reply callback and an event callback.
Callbacks are synchronous, bounded and non-reentrant. The event callback is the
logging boundary and receives failure, restart and stop reasons, plus the VM defect
while its frames are still available. The response contains the full handler result;
schema unwrapping and wire errors belong to MO-160.

`mo_service_new` evaluates every interval with the VM and refuses zero or negative
Durations before any tick is queued. It then evaluates the start value and queues
one startup tick per timer, in source order as space permits. Initialization uses
the same work and memory bounds as handlers. No environment variable is read.

`mo_service_send` retains typed arguments and samples their arrival time. It refuses
a full mailbox synchronously without retaining arguments. `mo_service_step` polls
timers, executes at most one queued message and polls again. UTC microseconds become
the handler's `Time`; a separate monotonic microsecond clock drives deadlines and
supervisor windows. A full mailbox delays a due tick until space is available.
Admission selects the oldest unqueued due tick, breaking ties in source order, so
recurring ticks cannot starve deferred startup ticks. Multiple elapsed periods
coalesce into one waiting tick. Deadlines advance past the
sampled time without looping over missed periods or overflowing the clock range.

The fixed defaults in `base/limits.h` are 64 queued messages and 1,048,576 VM
instructions per call. The existing controlled-call boundary counts one instruction
per bytecode dispatch, including calls and returns; a native primitive is one
instruction, not a wall-clock timeout. Memory is capped at 64 MiB per service process,
partitioned into the existing 8 MiB stack and 56 MiB heap. Retained state and queued
arguments count against that heap, so they reduce a message's remaining allocation
space. Shared immutable image data is outside this process budget. Mailbox and timer
metadata have a separate 8 MiB arena limit. These are internal defaults, not syntax
or configuration options.

The service retains its old state while a handler runs, preventing in-place mutation
of that state. Only a successful timer result, tuple or `Ok` tuple replaces it.
Read-only results and application `Error` results do not. After a defect or budget
failure, the loop reports the failed message, drops its live frame owners and reuse
tokens, and reuses the idle process for the next message. This cleanup is private to
services, whose handlers cannot receive capabilities. The public VM contract that
ordinary stopped or cancelled processes are terminal is unchanged.

The supervisor counts only defects and exhausted budgets. Its windows contain
events younger than 60 seconds or 10 minutes, respectively; an event exactly at the
window's age is expired. The fifth failure clears the failure window and records a
restart. The third restart inside its window records a stop too. Restart preserves
the last committed in-memory state and queued tools in FIFO order, discards queued
ticks and schedules fresh startup ticks. Stop refuses later submissions and replies
to queued tools with the internal down status. It never restarts automatically;
freeing and constructing a service is the explicit start operation at this stage.
`mo_service_new` retains this in-memory-only behavior. MO-158 adds
`mo_service_open`, which borrows a host store, restores its validated snapshot
and log before scheduling ticks, and commits updates before replying. A
supervisor restart keeps the in-memory state, which is always the last committed
state, so it does not reread the store. A storage failure stops the loop
without counting as a handler failure. The [saved-state format](saved-state.md)
defines the SQLite transactions, bounds, value encoding and refusal behavior.
This C embedding still exposes no Mo simulation API or service CLI command.

### MO-159 test simulation

`vm/simulation.c` owns the test capability's service loop, one open in-memory
store, and both clock readings. Its `SIMOP` family is VM-handled because host
cannot depend on VM. Rows in `hostops.def` have no fixed signature: checking
resolves a direct module/tool selector, and Core and bytecode validate its
indices and complete argument/result types against service descriptors. Only
construction is spec-only; an ordinary helper can operate on a supplied handle.
Core 0.9 and Lean's explicit evaluation boundary are specified in
`formal/TEXT.md` and `formal/TRANSLATION.md`.

The test and service heaps share an immutable image, never heap-owned values.
Every argument and reply crosses through the saved-value codec, including
read-only arguments and replies and application errors. This adds the saved
depth, node and byte bounds to those simulation-only transfers. The service's
own acceptance and recovery-memory checks remain in place. A reply-copy
failure cannot undo a committed update and must not invite an automatic retry.

Advance counts individual firings before moving either clock. Above
`MO_LIMIT_SIMULATION_TICKS` (10,000), it executes nothing. Otherwise it visits
each deadline and drains the existing loop there, preserving admission order
and the deadline's arrival time. No live-loop coalescing rule changes. Simulation
simply never jumps past a due deadline while the service is running.

Crash frees the loop; restart calls `mo_service_open` on the same still-open
store. Opening a new in-memory store would erase the evidence being tested.
The capability's once-only release frees the loop before closing the store,
including when the owning test heap is torn down after a bug or assertion.
Aliases share that ownership. These operation-boundary crashes complement,
rather than replace, MO-158's transaction and snapshot fault injection.

The test-only TaskQueue fixture wraps the unchanged `examples/job-queue.mo`.
Its submit/crash/restart/lease test exercises real recovery; ignoring an accepted
reply and retrying after recovery produces two jobs. Promise generators, the
MO-162 service, networking and deployment are outside this change.

### MO-160 private request processing

The embedded `McpRpc` module implements D322's pure request classification and
method validation. It uses `Json.parse`; duplicate object member names are refused
as unparseable. This includes decoded-equivalent names and duplicates inside tool
arguments. It does not invoke handlers or consume rate tokens. Notifications and
validated requests have different result variants.

RPC integer IDs retain their original spelling, including `-0`, within D317's
128-byte limit. String IDs are limited to 128 decoded UTF-8 bytes. Error envelopes
omit an indeterminate ID. Exact error messages remain caller-supplied in this
unit; D325's approved wording belongs to MO-160 part 2.

`Mcp.prepare` checks that every single-tool page fits the supplied response
ceiling with the worst-case escaped ID and its cursor, when needed. `Mcp.page`
counts serialized UTF-8 bytes, including the RPC envelope, and omits `nextCursor`
on the last page. These functions take the image key and response ceiling from
the caller; they do not freeze resource defaults. The private C embedding supplies
the loaded image key and `mo_version()`; embedded `McpTransport` owns HTTP status
mapping and D325's exact protocol errors.
D324 confirms D322 item 10: every notification that passes item 4, including one
with array params, receives HTTP 202 with an empty body and never runs a tool.
The regression-list line requesting HTTP 400 was a mistake. This unit returns
`Notification` for those envelopes; the HTTP mapping belongs to the listener.

### MO-160 node admission (D326 and D330)

Robert approved D330 on 30 Oct 2026 at 16:59 UTC. Both node-overflow paths return
HTTP 413 with an empty body, before consuming any rate token or admitting service
work. The native prescan refuses when Q + 1 exceeds 16,448, where Q counts opening
brackets, opening braces and commas outside strings. After parsing, Mo enforces
the exact limits of 16,384 payload nodes and 64 envelope nodes. Each Json value
counts as one node; object keys count toward bytes, not nodes. For `tools/call`,
only `params.arguments` is payload. Everything else belongs to the envelope;
methods without a payload must fit within 64 nodes. Wrappers and application
Bytes elements count. D330 assigns the response without changing those limits.

### MO-160 listener and client hookup

`tools/protocol.h` exposes the private C server constructor, polling loop and
metrics. It takes an already loaded service image, an open store, a listener and
explicit configured authorities. There is no `mo serve` command. The existing
service loop still owns admission, timers, supervision and durable updates.

The runtime-only listener family in `base/hostops.def` has real and fake
implementations. Real binds use numeric addresses, default to `127.0.0.1`, require
an explicit port (zero requests an ephemeral port), and require explicit consent
for non-loopback exposure. IPv6 listeners are IPv6-only. Native code owns socket
readiness, credentials and constant-time bearer authentication. Embedded Mo owns
header syntax, authority comparison, request policy, body framing, JSON parsing,
method validation, schemas and wire-value conversion. Authority comparison uses
ASCII host case folding and numeric ports, with default port 80. It performs no
DNS or alias expansion. Malformed authorities get 400, unconfigured authorities
421; absolute-form targets own their authority.

Eight connection slots remain occupied through a pending service reply. Polling
rotates slots and waits no later than the next service deadline. Headers and
bodies have absolute deadlines; fragments do not reset them. Each stage runs to
completion, so these deadlines are not a wall-clock execution interrupt. Final
responses close the connection, disable caching, half-close writing and drain
at most 512 KiB or 250 ms without parsing another request.

Authentication precedes body planning and JSON decoding. Header-stage values and
raw credential bytes are wiped before the authenticated request process starts.
Unknown tools and malformed params precede rate consumption. Every known-tool
call consumes a rate token before typed decoding, including invalid arguments.
The token-wide rate remains 20 calls per second with burst 40. Node overflows
precede both rate consumption and admission as D330 requires.

Tool results follow D319: success and application/runtime errors each carry one
compact object in both `structuredContent` and the text-content block. Both
copies count toward the complete response ceiling. `outputSchema` is a 2020-12
object union of success, application error when present, and runtime error.
`runtime/mcp-transport.mo` contains D328's fixed runtime messages and D325's fixed
protocol messages. The effects `none`, `applied` and `unknown` describe only this
call's service-state update, not retry safety. A lost updating reply has unknown
effect for the client.

Mo prepares fallback prefixes and suffixes at catalog time. Each slot reserves
a fixed buffer for the largest permitted fallback and a maximally escaped
128-byte ID. Native code splices only those exact bytes and uses precomputed HTTP
headers; fallback emission allocates nothing from an exhausted request heap.
Successful callback values are retained only until the poll loop projects and
serializes them. Native result packing retains its derived node guard,
`MO_LIMIT_MCP_RESPONSE_NODES = MO_LIMIT_MCP_RESPONSE_BYTES / 2 - 1` (16,383).
This guard is separate from the request payload and envelope node limits.
Serialization failure never retries an admitted operation.
Result rendering reuses the compact content text and rejects it before building
the duplicated RPC body when those two copies alone exceed the response ceiling.

Descriptor and catalog graphs are frozen during server construction and shared
immutably across requests. Their owning catalog heap outlives every connection
process. Configured authorities are frozen in that heap too, but each request's
header-stage process copies them into its own heap and charges their bytes to
that request. Request heaps, expanded byte lists, VM stack
scratch and temporary native arenas share the request memory allowance. Typed
copy-in temporarily constrains the destination service heap to the request's
remaining allowance, then restores the service limit before admission.

E0105 checks only wire arguments after State and Time, projected replies and
application errors, recursively. Private state and update-state wrappers are
excluded. E0106 rejects selected tool names ending in `?`; ordinary helpers and
timers remain unaffected. Both checks follow E0101 across all services (D328).

### MO-160 measurement accounting (D317 and D329)

VM work is bytecode dispatch, including one instruction per native primitive.
The transport additionally charges linear native byte/node visits; it does not
claim to meter each C table probe or comparison as a VM instruction. Native
scanning, copying and conversion have byte, node, depth and iteration bounds,
and their wall time must be measured. Hashing and typed Map/Set duplicate checks
remain in the service image's runtime, not JSON equality.

`request_memory` includes the connection resident bytes, the protocol-process
lifetime peak and a conservative bound for temporary service copy-in. Native
scratch telemetry counts aggregate arena reservations, including capacity and
allocator headers, across simultaneous arenas. `native_memory` adds connection
resident bytes and these scratch peaks; the service copy-in contribution may
conservatively include earlier heap activity. `concurrent_memory` sums active
connections' lifetime peak bounds, not a sampled simultaneous-live peak. Catalog
memory is separate. Compiled images, the service's existing allowance and SQLite
storage are not claimed as transport memory or total RSS.

The original D329 working byte ceilings failed the required 2× matrix. The
measured defaults lower the header block from 2 KiB to 1 KiB, decoded body from 256 KiB
to 16 KiB, complete response from 256 KiB to 32 KiB, and encoded ingress from
512 KiB to 24 KiB, including headers. The allowances remain 1 MiB/1M work for
pre-authentication inside 16 MiB/32M work per request, and 64 MiB for catalog
preparation. Node, depth, connection, rate and type-graph limits are unchanged.
The complete matrix passes with these reductions under strict GCC `-O2`:
bytewise pre-authentication, byte-fit values and duplicates, depth 60/61, escaped
results, maximum responses, catalog pages, 1,024 chunks and eight retained peak
connections. Original node-ceiling requests exceed the lower body ceiling and
are refused before parsing; the pure transport fixtures retain the exact node
boundary checks. Results and timings are recorded in the PR. Raising any limit
or adding memory/work requires Robert's approval.

## 10. Done means

Mo's own agents take every task from the job queue for one week with no restart, and a deliberate
crash in the middle of that week loses no accepted job (D260). This replaces the earlier finish
line of 24 hours under synthetic load.
