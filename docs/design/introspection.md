# Introspection and replay: the Phase 4 design

**Status: implemented; diagnostic gate passed on 2026-11-07 (14 of 16 correct). Robert approved v0.1.0 on 2026-11-07.**
The [run record](../status/phase4-run.md) reports both misses and the limits of the evidence. Section 12 records the
decisions made at approval, and "Approval and changes" at the end records every later change. A
change to an interface, format or limit here is made in this document, with its reason, before
the code that depends on it lands. The phase plan is
`docs/plans/2026-11-03-1704-feat-phase4-see-inside-plan.md`; this document holds the interfaces,
budgets, models and experiment results it asks for. The original evaluation protocol is `learnability/phase4/protocol.md`; its historical-status notice and the phase plan’s R20–R24 govern the 2026-11-05 evaluation amendment. The experiments this design rests on are in
`learnability/phase4/preflight/`, with their results in its `README.md`.

## 1. What Phase 4 promises

An agent operating a running Mo service gets three instruments, each with a narrow promise.

1. **Live views.** Built-in operator tools on the service's existing MCP endpoint show its state,
   mailbox, timers, memory, supervisor events, retained failures and committed messages since the
   last snapshot. A token holder can read them. They never change the service. The one
   exception is the live upgrade tools (`docs/design/upgrades.md`), which replace the program and
   require a second credential the service token never stands in for.
2. **Offline queries.** The service exports bounded, identified bytes of its state, or of one
   retained failure, to a private file. The agent runs a pure Mo function over that file on its
   own side. The service never runs caller-written code.
3. **Failure replay.** A handler failure that the supervisor counts leaves a diagnostic record
   when capture succeeds, or an explicit "record unavailable" with a reason when it does not.
   Offline, the agent replays the record against the original compiled program and runtime
   build, reads the stack and variables, and checks a revised handler against the reconstructed
   case.

What none of them promises:

- **Live memory.** A replay reconstructs the handler's logical inputs: its state, arrival time
  and arguments, the program, the runtime, and the recorded limits. It does not reconstruct the
  live heap's other occupants. Queued mailbox arguments share the handler's heap
  (`src/vm/service.c`, `MO_LIMIT_SERVICE_MESSAGE_MEMORY`), so a handler can run out of memory
  live and succeed offline under the same ceiling. Replay reports that as **not reproduced**.
- **What a fix verdict covers.** A verified fix means the original failure reproduced offline
  and a compatible revised handler then succeeded under the same reconstructed conditions. It
  says nothing about live memory pressure or future workloads.
- **History before the last snapshot.** The recovery log keeps at most the messages since the
  last snapshot (section 9).

## 2. Where the operating agent stands

The live views need only the service's URL and token, so a client behind the exe.dev proxy can
use them. Fetching an export, replaying and querying need more: the agent must run commands on
the service's host, or on a machine holding the data folder, with the program's source and the
runtime build that recorded it. A remote operator reaches the host over SSH, as the Phase 3
runbook did (`docs/ops/task-queue.md`). No TLS client is added, and fetch refuses anything but
loopback (section 5.3).

The Phase 3 deployment host `turnpike-ferret` was deleted on 2026-11-03 (`docs/ROADMAP.md`), so
there is no remote host to demonstrate access to. The evaluation therefore runs each service
variant on the same machine as the agent session (`learnability/phase4/protocol.md`, "Where
sessions run"). That machine meets the prerequisite by construction: the agent can read the
source, run the pinned `mo`, and reach `127.0.0.1`. A remote, proxy-only deployment remains a
documented limit: it gets the views and nothing else.

## 3. Operator tools on the MCP endpoint

### 3.1 Names, order and annotations

| Wire name | Shows | Arguments |
| --- | --- | --- |
| `mo-overview` | Service name, program and build identities, uptime, running or stopped, mailbox count, timer count, memory used and peak, failure and restart counts in their windows, retained failure count, recovery sequence, last snapshot sequence | none |
| `mo-state` | The current state, rendered from its saved encoding, truncated to fit one reply | `path` (optional, a list of field names and indexes to descend into) |
| `mo-mailbox` | Each queued message: handler, timer or tool, arrival time, argument byte size; not argument values | `offset` (optional position in the queue) |
| `mo-timers` | Each timer: name, interval, next due time, whether a tick is waiting | `offset` (optional timer position) |
| `mo-memory` | Heap used, peak and limit; stack limit; encoded state bytes; pinned export bytes; diagnostic scratch peak | none |
| `mo-supervisor` | The in-memory event ring: failures, restarts, stops and capture refusals, newest first | `after`, `before` (optional event numbers) |
| `mo-log` | Committed messages since the last snapshot: sequence, handler, arrival time, argument byte sizes; not argument values | `after` (optional sequence) |
| `mo-export` | One page of a pinned export (section 5) | `kind`, `failure`, `export`, `offset` |
| `mo-failures` | Retained failure records: id, handler, kind key, arrival time, signature, state and message byte sizes; unavailable records with their reasons | `after` (optional record id) |

Operator tools are listed after every author tool, in the order above, under the same catalog
paging (`runtime/mcp.mo`). A list view that does not fit one reply carries `"truncated": N`, the
number of items left out, and `"next"`, the arguments that return the following page. An
argument number must be written as a plain nonnegative integer, so `-0`, `1.0` and `1e2` are
refused. A hyphen cannot occur in an author tool's name, which the lexer limits
to `[a-z0-9_]` (`src/syntax/lexer.c`), so the `mo-` prefix cannot collide on the wire.

Clients may rename tools before a model sees them, and the design does not depend on spelling
surviving. The client probe (`learnability/phase4/preflight/README.md`, "Client names and
approvals") ran an author tool `mo_state` beside an operator tool `mo-state` through a recording
proxy:

- Codex 0.166.0 showed the model `mcp__probe__mo_state_94338136a5cf` and
  `mcp__probe__mo_state_c5d0a7c9f588` for the two, and `mcp__probe__mo_overview` for
  `mo-overview`, as its source says it would. Each call still reached the intended wire tool.
- Claude Code 2.1.317 kept the hyphen: `mcp__probe__mo-state` beside `mcp__probe__mo_state`.
- Codex ran the two tools carrying `readOnlyHint` without asking. With
  `approval_policy="never"` it refused the unannotated author tool ("MCP tool call requires
  approval, but approval policy is never"). With `mcp_servers.NAME.default_tools_approval_mode="approve"`
  it ran all three.

Every operator view therefore carries `readOnlyHint: true` and `idempotentHint: true`, and
`mo-export` also `openWorldHint: false`. Descriptions are at most 512 bytes each and say what the
tool shows and its limits. Author tools gain no annotations in this phase: the existing
`read_only` field of `McpType.Tool` is still not emitted, so an author's read-only tool is not
silently given an approval-skipping hint. The evaluation configures client approval explicitly
(`learnability/phase4/protocol.md`).

The nine views are followed by `mo-upgrade-open`, `mo-upgrade-page` and
`mo-upgrade-commit`. These are the one documented exception to operator read-only
safety: open reserves an upload slot, page writes consecutive padded-base64
ranges (8192 decoded bytes except the final page), and commit replaces the
program and migrates state at a message boundary. Every call needs the service
token plus `Mo-Upgrade-Authorization: Bearer VALUE`; the service token alone
never authorizes an upgrade. All three carry `readOnlyHint: false`,
`idempotentHint: false` and `openWorldHint: false`; only commit carries
`destructiveHint: true`. Their descriptions and schemas specify the page fields,
base/candidate digests and slot lifetime. See `upgrades.md` for the shared reports.
Remote pushes use the SSH local-forward procedure in `../ops/task-queue.md`.

### 3.2 Dispatch

`McpRpc.validate` (`runtime/mcp-rpc.mo`) learns the operator names. A call to one is routed to a
new native entry point instead of `mo_service_send`. It spends the token's rate allowance exactly
as an author call does, so operator calls cannot flood the listener. Operator calls:

- run only at service idle points, between `mo_service_step` calls, through new `vm.h`
  accessors (failure and restart counts and windows, recovery and snapshot sequence, timers,
  mailbox entries' handler, kind, arrival and byte sizes, the encoded current state, the event
  ring, the failure store);
- never enter the mailbox, the recovery log or supervisor accounting, and never call a Mo
  handler;
- answer when the mailbox is full. After a supervisor stop, `mo serve` closes its listener as
  it does today, so a stopped service is read with `mo history` (section 9).

JSON shaping is Mo code in a new `runtime/mcp-views.mo` beside the existing MCP runtime, following
implementation condition 4: the C side hands it typed records and byte spans, and it builds the
JSON value.

### 3.3 Rendering state

The existing MCP shapes cannot render every legal state. Shape reachability starts at tool
arguments and results (`src/bytecode/service-reach.c`), so a service whose tools are all
read-only has no shape for its state, and the converter asserts a shape exists
(`src/tools/protocol-value.c`). Private state is legal, and E0105 deliberately skips it.

The state view is rendered from the **saved encoding** instead. Every saved value carries its own
complete schema: tags, nominal names (already qualified by module), case and field names
(`docs/design/saved-state.md`, "Encoded values"). A schema-driven renderer in Mo turns those bytes
into JSON:

| Saved type | JSON |
| --- | --- |
| Unit, Bool | `null`, `true`/`false` |
| Int, Byte, BigInt, Duration | a decimal string, as author tools render Int today; Duration as microseconds |
| Float | a number, or the strings `"NaN"`, `"Infinity"`, `"-Infinity"` |
| Decimal | a decimal string |
| Char, Text | a string |
| Time | an RFC 3339 UTC string, as author tools render Time |
| Bytes | `{"base64": "..."}` |
| Range | `{"start": "...", "end": "..."}` |
| List, Set | an array |
| Map | an array of `[key, value]` pairs, since keys need not be strings |
| Tuple | an array |
| Record, struct | `{"type": "Module.Name", "fields": {...}}` |
| Choice | `{"type": "Module.Name", "case": "Name"}`, plus `"value"` for one unnamed payload or `"fields"` for named ones, as author tools render a choice |

A struct is saved as a choice with one case named after its type, and the saved schema has no
other mark of it, so the renderer treats such a choice as a struct.

The bytecode format, the shape tables and the verifier are unchanged. The renderer reads only
bytes the runtime itself encoded from a trusted descriptor, under the depth (128) and node
(1,048,576) limits the codec already enforces. This also shows private state to token holders.
That is intended: module privacy governs which source can name a representation, not what the
service's operator can read, and the operator already holds the only credential.

A view that would exceed the reply limit is truncated. The renderer counts every byte it writes,
separators included, against a byte budget, and the reply carries `"complete": false`.

- A cut list, set, map or tuple becomes `{"items": [...], "truncated": N}`, and a cut record or
  choice gains `"truncated": N`. N counts the elements or fields left out.
- A Text, Bytes or BigInt too large for what is left becomes `{"truncated_bytes": N}`, its
  encoded size, and rendering continues with the values after it.

`path` lets an agent descend into a truncated part without exporting. Following a path skips
earlier values without rendering them, in one step when every element has the same encoded size.

### 3.4 Event lines

The three stable `mo serve:` line shapes do not change (`docs/design/services.md`, "MO-161 mo
serve"). After a `failure:` line, the indented report always ends with exactly one of:

```text
  record: 42
  record unavailable: REASON
```

`REASON` is one line from a fixed set (section 6.4). A work-limit failure, which today has no
report at all because a cancelled call fabricates no defect, gets a report consisting of its
stack (section 6.2) and that line.

## 4. Identities

Four identities, each checked separately.

| Identity | What it is | Today |
| --- | --- | --- |
| Artifact format | A magic and version on every export (`MOEX` 1); failure records use the store's own `MOST` 1 frames with two new kinds, and their message payload carries its own format version | new |
| Program | FNV-1a-64 of the checksum-covered bytecode without the `files` section | exists: `mo_image.program_checksum`, folded into the store's image key |
| Runtime build | SHA-256 of the running `mo` executable's bytes, read once from `/proc/self/exe` at startup (or the platform's equivalent), plus a build label: `mo --version`, the compiler and its version, and the build configuration (`debug`, `release`, `sanitize`) | new; `mo --version` prints only `mo 0.0.0` today |
| State schema | SHA-256 of the state type's saved schema bytes | new; the schema bytes themselves are already in every encoded value |

The build label is for people; only the digest decides. Hashing the executable is the honest
reading of "the same runtime build": any change to the compiler, its flags or the embedded MCP
runtime changes it. FNV-1a-64 stays the record checksum, as in the store, and detects accidental
corruption. SHA-256 here detects that two identities differ; neither is a signature, and nothing
in this phase claims authenticity. SHA-256 is a new primitive in `base/`, checked against the
FIPS 180-4 examples and fuzzed against a reference implementation in the unit tests.

**Keeping the original build.** v0.1 replays only on the build that recorded the failure. The
operator keeps that `mo` executable, as the Phase 3 runbook already pins `/opt/mo/$SHA` per
deployment. A replay on another build is refused with the recorded build label and digest, so the
agent knows which executable to fetch. Records and stores written before Phase 4 carry no build
identity: their failures were never recorded, their state can be read by `mo history`, and a state
query needs an export from a Phase 4 build (section 7.4). Nothing can be replayed from them. Replay
also needs the recording executable to contain `mo replay`, so records captured by a build that
predates the offline commands are evidence only: `mo history` lists them as not replayable here.

## 5. Export

### 5.1 Pins

`mo-export` with `kind: "state"`, or `kind: "failure"` and a record id, creates a **pin** at the
next idle point. A pin owns immutable bytes: the complete artifact (section 5.2). The service
keeps the encoded current state in native memory already (section 6.3), so a state pin copies
those bytes; it never runs the codec. A failure pin reads its record from the store.

- Each pin has a 128-bit **export id** from the operating system's random source, independent of
  the recovery sequence and unique across replacements and restarts. A replacement can carry the
  same sequence as the pin it replaces; it never has the same id.
- At most `MO_LIMIT_EXPORT_PINS` (2) pins exist, one per kind. A new export of a kind replaces
  and releases that kind's pin.
- A pin expires `MO_LIMIT_EXPORT_IDLE_US` (60 seconds) after its last page was read. A pin is
  released after its last page is read, on replacement, expiry, supervisor restart or process exit. Pins are never
  persisted, so a restart releases every pin.
- Pin bytes are charged to `MO_LIMIT_EXPORT_BYTES`, a budget separate from the handler heap:
  2 × (`MO_LIMIT_SAVED_BYTES` + 1 MiB of metadata). A pin that does not fit is refused with
  `export_limit`, not allocated from the heap. Pins share no heap value with the service, so
  handler allocation and in-place reuse are unaffected.
- Progress is not guaranteed: another client can replace a pin mid-fetch. Fetch reports
  `replaced` and the operator retries.

### 5.2 The artifact

All integers little-endian, as in the store.

| Field | Bytes | Meaning |
| --- | ---: | --- |
| Magic and version | 5 | `MOEX` 1 |
| Kind | 1 | 1 state, 2 failure |
| Export id | 16 | Random, per pin |
| Producer program | 8 | Program checksum |
| Producer build | 32 | Runtime build digest |
| Build label length, label | 4 + n | UTF-8, at most 256 bytes |
| Service descriptor index | 4 | As in the image key |
| Recovery sequence | 8 | Durable head sequence at pin time |
| Encoded-state sequence | 8 | Sequence of the cached state bytes; may trail the durable head after snapshot failure |
| Created | 8 | UTC microseconds |
| State schema | 32 | SHA-256 of the state schema bytes |
| Body length | 4 | Exact |
| Body | variable | Kind 1: one encoded state value (`MOSV`); kind 2: one failure record pair (section 6.1) |
| Artifact digest | 32 | SHA-256 of every preceding byte |

### 5.3 Pages and fetch

A reply's compact content may use at most half of the 32 KiB reply limit, because it is sent
twice (`runtime/mcp.mo`, `tool_reply`), and it is base64. A page therefore carries
`MO_LIMIT_EXPORT_PAGE_BYTES` (8 KiB) of artifact bytes. Each page reply carries the export id,
kind, artifact version, recovery and encoded-state sequences, offset, page length, total length,
page count and the page's
FNV-1a-64; the final page also carries the artifact digest. A page of a released, replaced or
expired pin is refused with `export_released`, and a page for an id that does not match the
current pin with `export_replaced`; neither ever returns another pin's bytes.

A 16 MiB saved state plus framing is 2,049 pages. At the token's rate limit of 20 calls a second that is about
103 seconds, and the fetch tool says so before it starts. A failure pin is at most
`MO_LIMIT_EXPORT_BYTES / MO_LIMIT_EXPORT_PINS` (17 MiB), which is 2,176 pages, about 109 seconds.

`mo fetch URL --export state|failure ID --out FILE --token-env NAME`:

- accepts only `http://127.0.0.1:PORT/mcp`, `http://localhost:PORT/mcp` and `http://[::1]:PORT/mcp`;
  refuses userinfo, other schemes, other hosts and non-numeric ports before reading the token;
- resolves `localhost` internally to numeric `127.0.0.1` and `::1` addresses without DNS/NSS,
  connects only to a loopback address, and checks the connected
  peer address is loopback on every connection;
- ignores proxy environment variables entirely and follows no redirect: any status other than
  200 ends the fetch;
- bounds the response head (1 KiB), body (`MO_LIMIT_MCP_RESPONSE_BYTES`), page count
  (`total / page + 1`), total bytes (`MO_LIMIT_EXPORT_BYTES / 2`) and wall time
  (`MO_LIMIT_FETCH_US`, 10 minutes);
- permits one lookahead request only after accepting the first page. It uses the accepted
  generation and predetermined bounded offset, validates the new peer before credentials,
  caps its socket receive buffer and still admits all responses in exact order. This overlaps
  server rendering with client parsing; an aborted attempt closes the pending socket;
- writes to a temporary file created with `O_CREAT | O_EXCL | O_NOFOLLOW` and mode 0600 in
  `FILE`'s folder, checks every page's metadata matches the first page, the coverage is exact
  and the digest matches, then publishes it with `link` (which refuses an existing `FILE`) and
  removes the temporary name; any failure removes the temporary file. An existing `FILE` or a
  symlink is refused before connecting.

Exit statuses follow `mo`'s: 0 published, 1 refused by the service or by a check, 2 a command
mistake. Response parsing is Mo code (the existing `Http` and `Json` modules); sockets, files and
the digest are C. A new fuzz target, `fuzz_fetch`, feeds response sequences through the reader
with its own seeds.

## 6. Failure records

### 6.1 Storage

The store gains two tables, created by `mo_store_open` alongside the three it already checks, in
the same transaction:

```sql
CREATE TABLE failure(id INTEGER PRIMARY KEY, kind TEXT NOT NULL, first INTEGER NOT NULL,
  message BLOB NOT NULL) STRICT;
CREATE TABLE failure_state(id INTEGER PRIMARY KEY, record BLOB NOT NULL) STRICT;
```

`user_version` stays 1, and the existing tables and their records are unchanged. The store's
check is by table name (`src/host/store.c`, `table`), so a Phase 3 binary opening a Phase 4 store
ignores the new tables, and a Phase 4 binary opening a Phase 3 store creates them.

A record is two framed blobs bound to one id, in two rows inserted, with any evictions, in one
transaction:

- **state**: in `failure_state`, a frame of kind 3 holding the encoded pre-call state;
- **message**: in `failure`, a frame of kind 4 holding the failure metadata below.

They are separate rows because SQLite applies `SQLITE_LIMIT_LENGTH` to a whole row as well as to
each value, and the store sets it to `MO_LIMIT_STORE_ROW`, 16 MiB plus 4,096 bytes
(`src/host/store.c`, `mo_store_open`). One row holding a near-limit state and its message would
pass that limit; a state row alone fits it exactly as a snapshot row does, and the message row is
at most 1 MiB. Both frames are the store's own envelope, `MOST` 1, with kinds 3 and 4 beside the
log's 1 and the snapshot's 2 (magic, kind, sequence, image key, length, payload, FNV-1a-64), so
`mo_store_record_read` reads them by kind as it reads the others. The message payload, in order:

1. format version (1); failure id; recovery sequence at the failure; arrival time;
2. timer flag, handler table index, handler name;
3. read-only flag;
4. argument count and each argument as a length and encoded value, exactly as a log record;
5. the structured signature (section 6.2);
6. the recorded budgets: work, heap, stack and depth limits, and the saved-bytes limit;
7. the store image key (the program checksum with the service index folded in, as in every
   `MOST` frame), runtime build digest and label, state schema digest;
8. diagnostic context: up to `MO_LIMIT_FAILURE_FRAMES` (8) frames (function, file, span), up to
   `MO_LIMIT_FAILURE_VARIABLES` (32) top-frame variables, and a flag for the whole context
   (`complete`, `truncated`, `unavailable`). Each variable is marked
   `available` with its rendered value or `unavailable` without one. A value whose rendering
   exceeds `MO_LIMIT_FAILURE_VALUE_BYTES` (1 KiB) is `unavailable`, never cut short, and marks the
   context `truncated`. Every value, top-level or nested, is quoted and escaped the same way, so
   no value puts a raw NUL or newline into the context.

Items 1 to 7 are the replay inputs and must be complete for a record to exist. Item 8 is
context: replay regenerates the stack itself.

### 6.2 Signatures

| Class | Fields | Matches when |
| --- | --- | --- |
| `defect` | VM reason (a fixed string such as `integer overflow`, never the program's own `stop` text), function, file, span | class, reason, function, file and span are equal |
| `work` | limit name and value | class and value are equal |
| `memory` | which limit (`--max-heap`, `--max-stack`, `--max-depth`) and its value | class, name and value are equal |
| `state_size` | which saved-copy limit (bytes, depth or nodes) and its value | the new state's saved copy failed again, under the same limit name and value |

The signature has its own version (1). Rendered report text is never compared. A generic
"out of memory" from a different limit is a different outcome, not a match.

The **kind key** groups records for retention: handler name, class, reason, and function and span
for defects. It is stored as text in the `kind` column.

### 6.3 Capture

Order, at the point where `mo_service_step` sees a failed call today:

1. **Context, before cleanup.** The frames are still live: the event hook already runs before
   `finish_call` unwinds them (`src/vm/service.c`). For a defect, the existing defect frames are
   used. For a work-limit cancellation, which fabricates no defect, the runtime walks the
   process's live frames the same way. Frames and top-frame variables are rendered as text
   into a buffer of at most `MO_LIMIT_FAILURE_CONTEXT_BYTES` (64 KiB, the defect report's size),
   allocated outside the heap. Frame lines need no heap, but rendering a variable's value can.
   `mo_value_print_literal` takes scratch from the handler heap for values nested more than 16
   levels deep, and BigInt text is built there too. After a heap exhaustion such a value comes
   out `unavailable` and the context is `truncated`. The frames and the replay inputs are
   unaffected.
2. **Cleanup.** `finish_call` runs as today, releasing the handler's frames and garbage. The
   pre-call arguments stay alive: the message still owns them.
3. **Encoding.** The pre-call state's encoding is already in hand (below). Arguments of an
   updating tool were encoded at admission; the runtime now keeps those bytes with the queued
   message instead of discarding them. A read-only tool's arguments are encoded now, after
   cleanup, with scratch charged to `MO_LIMIT_FAILURE_SCRATCH` (1 MiB), not the heap. Read-only
   admission is unchanged: arguments that cannot be encoded make the record unavailable, not the
   call refused.
4. **Store.** The pair and any evictions go in one transaction. The record id is published only
   after commit.
5. **Supervisor and reply**, as today: the failure is counted once.

**Keeping the current state encoded.** A store-backed service already passes every accepted
state through encode and decode before publishing it (`docs/design/saved-state.md`, "Encoded
values"). The runtime keeps the encoded bytes of the current state, in native memory outside the
heap, until the next accepted state replaces them. This is what makes capture possible after a
heap exhaustion, and it makes a state pin a copy. It costs up to 16 MiB of resident memory beside
the 64 MiB process budget; section 10 counts it.

### 6.4 When capture fails

| Outcome | Record | Service |
| --- | --- | --- |
| Committed | `record: ID` | continues under the existing policy |
| The service has no store, or no encoded current state | `record unavailable: persistent state unavailable` | continues |
| Prepared value too large: state plus arguments plus metadata over `MO_LIMIT_FAILURE_RECORD_BYTES` less the 512-byte envelope reserve | `record unavailable: too large to record` | continues |
| The kind key is longer than `MO_LIMIT_FAILURE_KIND_BYTES` (1,024 bytes) | `record unavailable: failure kind too long` | continues |
| Read-only arguments could not be encoded within the scratch limit | `record unavailable: arguments could not be encoded` | continues |
| Context buffer or record buffer could not be allocated | `record unavailable: out of memory for the record` | continues |
| The runtime build digest or the state schema digest could not be computed | `record unavailable: runtime identity unavailable` | continues |
| The built message does not decode with the message reader (`mo_failure_read`) | `record unavailable: record failed validation` | continues. Nothing is written |
| Retention has no room: a repeat of an existing kind fits only by evicting another kind (section 6.5) | `record unavailable: the store refused the record` | continues |
| The stored diagnostic rows are damaged: a record missing one of its two rows, a kind that is empty, longer than 1,024 bytes or not UTF-8, a `first` flag other than 0 or 1, or more than 64 pairs | `record unavailable: diagnostic records damaged` | continues. Listing and exporting those rows still report `corrupt` |
| The diagnostic transaction failed and rolled back cleanly (`SQLITE_FULL`, `SQLITE_TOOBIG`, `SQLITE_NOMEM`, `SQLITE_CONSTRAINT`, `SQLITE_BUSY`, `SQLITE_LOCKED`), and the connection still answers a read of `head` | `record unavailable: the store refused the record` | continues. The store's existing failure mapping turns `SQLITE_TOOBIG` into `MO_STORE_CORRUPT`, which is right for reading saved data and wrong here: the record write classifies it as a clean refusal |
| `COMMIT` returned an error, so the pair may or may not be durable; the rollback failed; any other SQLite error, such as `SQLITE_CORRUPT` or `SQLITE_IOERR`; or the connection cannot read `head` | `record unavailable: store outcome uncertain`. The reply names no record id; after a failed `COMMIT`, a later reopen can still find the pair | stops under the existing store-failure policy (`MO_SERVICE_STORE`, exit 70) |

The handler failure is counted once in every row. A capture refusal is never counted as a second
handler failure, never retried, and appears in `mo-supervisor` and `mo-failures`. An application
`Error` reply, an admission refusal (`BUSY`, a message too large to log) and a down service record
nothing. A read-only call that fails gets a record like any other; its state and the recovery log
are unchanged. The final failure that stops the service is captured before the stop event and
the exit.

`MO_LIMIT_FAILURE_RECORD_BYTES` is `MO_LIMIT_SAVED_BYTES` for the state plus 1 MiB for arguments
and metadata. Under `mo serve`, every tool's arguments arrive in a request body of at most 16 KiB
(`MO_LIMIT_MCP_BODY_BYTES`), so the 1 MiB only matters to embeddings. An updating message whose
arguments use the whole saved-bytes allowance is legal, and its record is then too large: that is
an explicit refusal, and message admission does not tighten to avoid it. A complete record can
approach 17 MiB, so a record transaction writes up to about 34 MiB with its journal; the store
budget (`MO_LIMIT_STORE_MEMORY`, 24 MiB per store) was sized from a 16 MiB record.
`test_failure_sizing` measures it: three transactions of a maximal pair (first, latest, then a
replacement latest; 17,825,348 framed bytes each) all commit within that budget. The process
peaked at 56,644 KiB of RSS from a 21,592 KiB baseline, including native input and frame
buffers, and the database file held the two retained pairs in 35,708,928 bytes
([`sizing.json`](../../learnability/phase4/failure-capture/results/sizing.json), one VM and
debug build).

### 6.5 Retention

At most `MO_LIMIT_FAILURE_RECORDS` (64) records and `MO_LIMIT_FAILURE_STORE_BYTES` (64 MiB of
framed rows) are kept. Each kind key keeps at most two records: its **first**, kept as long as
its kind is, and its **latest**.

A new failure of an existing kind deletes that kind's previous latest, if it has one, and takes
its place. It never evicts another kind. If the new record still does not fit, the capture is
refused as `the store refused the record`, the transaction rolls back and the previous latest
stays. A repeat fails to fit when its kind had only a first record and 64 records are already
kept, or when it is larger than the bytes left. So once 64 kinds each hold only a first record,
every repeat is refused until a new kind's arrival evicts one of them. A timer that keeps failing
with room to spare rewrites its one latest row.

When a new kind arrives and either limit would be exceeded, the kind whose latest record is
oldest is evicted whole, first and latest together. Eviction repeats until the new record fits.
The views count evicted kinds for the current open store handle. A single record larger than the
byte limit is refused as too large to record. Snapshots never touch the failure tables, so a
retained failure stays replayable after any number of snapshots.

### 6.6 The event ring

`MO_LIMIT_SERVICE_EVENTS` (256) in-memory entries record failures (with their record id or
refusal reason), restarts, stops and capture refusals, each with a time and a number. The ring
is lost when the process exits; stopped history (section 9) shows only what is persisted.

## 7. Offline commands

### 7.1 Commands

```text
mo replay FILE --failure ID (--data DIR | --export ARTIFACT) [--json]
mo replay FILE --failure ID (--data DIR | --export ARTIFACT) --fix REVISED [--json]
mo query FILE FUNCTION --export ARTIFACT [--max-work N] [--max-heap N] [--json]
mo query FILE FUNCTION (--data DIR | --export ARTIFACT) --failure ID [--json]
mo history DIR [--json]
```

`FILE` is the program's main module, as for every `mo` command. `replay` compiles `FILE` and
requires its program checksum to equal the record's, and refuses any other runtime build (section
4). It decodes the record and runs the handler through the same controlled call the service uses,
factored out of `src/vm/service.c`, with the recorded budgets, no timers, no mailbox and no
store. It prints the original message, pre-call state, signature and the regenerated stack with
top-frame variables, and marks fields unavailable or truncated.

The recorded budgets are compared with the recording build's own limits (the service work limit,
the service process's heap, stack and depth limits, and the saved-bytes limit) and a record whose
budgets differ is refused. Replay requires the same build, so a differing budget means a damaged or
crafted record; replay never runs with budgets read from the file. `FILE` must be the untouched
source that recorded the failure: any byte change in any module, a comment or `mo format`
included, changes the program identity, and the refusal says so.

`--fix REVISED` first replays the original as above, then runs the handler of the same name from
`REVISED` on the same inputs and budgets. `REVISED` is a main module whose sibling modules resolve
from its own folder. Every compatibility check of section 7.4, for both programs, runs before
either handler runs, so a revision that changes the state type is refused before the original
replays; `mo replay` without `--fix` still shows whether the original reproduces. The new state of
an updating tool, and of a timer, whose result is the new state, must also pass the saved-copy size
check. An `Error` reply is a success: live, it records nothing and is not a handler failure. The
revised outcome reports its reply kind (`ok` or `error`) and whether the state changed, so
`verified` is not read as "now does the work". The verdict is:

| Original | Revised | Verdict |
| --- | --- | --- |
| reproduced | succeeded, and an updating tool's or timer's state passed the size check | `verified` |
| reproduced | failed or exceeded the recorded budget | `not_fixed` |
| different outcome or not reproduced | succeeded | `unverified` (the revised run is reported as an observation) |
| different outcome or not reproduced | failed | `unverified` |
| refused | not run | `refused` |

Both runs' outcomes and identities are reported together. A `verified` verdict is never
produced without a reproduced original.

`mo query FILE FUNCTION` runs a pure function of the program in `FILE`:

- over a state export: `FUNCTION(state)`;
- over a failure: `FUNCTION(state, time, arguments...)` in the handler's order.

The result is printed in `mo`'s structural text form, or as JSON with `--json`. A query is not a
handler, and only `replay --fix` gives a verdict.

### 7.2 Query entry rules

The entry is a non-generic top-level function of `FILE`'s main module, or `Module.name` of a
module the program uses, whether or not that module exposes it: choosing an entry is an operator
action like `mo test --only`, and module privacy already governs the entry's body. A generic entry
is refused, since lowering keeps only the instances a program uses. The name is resolved through
the checker's symbols, so an overloaded name selects the overload whose first parameter is the state
type. The checker refuses an entry whose parameters or result contain a capability or a function,
including inside records, choices, tuples and collections: the rule the checker already applies to
service state and handler inputs (reference section 11.5, `src/check/services.c`), applied to the
entry. A named top-level function captures nothing, so its parameters and result are the whole
check. The entry's first parameter must be the state type of `FILE`'s main service exactly. A
failure query's remaining parameters must be `Time` and the handler's argument types, in order.
Normal module privacy applies: a query outside the state's defining module uses its exposed
accessors, and a query of a private representation lives in the defining module.

The query prototype (`learnability/phase4/preflight/README.md`, "Queries from another program")
showed the decoder already accepts this: a program that adds a pure function to `JobQueue`, and
one that adds a `Query` module reading `JobQueue.jobs`, both decoded the task queue's saved state
once the whole-program image key was taken out of the comparison, while renaming one state field
was refused.

### 7.3 Results and exit statuses

`--json` writes one JSON object per line, like `mo test --json`: first `{"record":"version",
"format":"1.0","mo":...,"command":...}`, then one object per outcome (`original`, `revised`,
`query`, `history`), then `{"record":"totals",...,"status":N}`. The totals record keeps `mo test
--json`'s `status` field for the exit status. Field names keep their meaning within a major version.
Values in JSON are rendered from the saved schema alone, by the same embedded renderer as the live
views. History and the live views use the same field names for log entries, failures and events,
with one exception: a log row stores only its handler's index and timer flag, and `mo history`
reads no program, so a tail entry's `handler` is null, `handler_index` carries the index, and the
text form says the name is not recorded in the log.

Exit statuses: 0 when the requested result holds (`reproduced`; `verified`; a query that
returned; history read); 1 when it does not (`different`, `not_reproduced`, `not_fixed`,
`unverified`, or a refusal); 2 for a mistake in the command; 70 when a query's own function stops,
with the report `mo run` prints for a bug (reference section 13.2).

### 7.4 Compatibility

Each check is separate and runs before any value is decoded or any handler runs.

| Command | Format | Program | Runtime build | State schema | Handler inputs |
| --- | --- | --- | --- | --- | --- |
| `fetch` | supported version, one export id, exact coverage, digest | recorded only | recorded only | recorded only | — |
| `replay` | supported | must equal | must equal | must equal | must equal (same program) |
| `replay --fix` | supported | original must equal | must equal | revised must equal | revised handler: same name, timer or tool, same argument types in order, same result shape class (timer, read-only, updating) |
| `query` over state | supported | may differ | v0.1: must equal the producer's | must equal | — |
| `query` over a failure | supported | may differ | must equal | must equal | entry parameters must equal state, `Time` and the arguments in order |
| `history` | supported store | not read | shown per failure record | shown, not checked | — |

`mo history` takes no program. It renders values from the saved schema alone, so it needs no
runtime types. Snapshots and log rows carry no build identity, so the snapshot is shown with
"producer build not recorded"; each failure record shows its own build label and digest and whether
this build can replay it.

The checks run in a fixed order: format, kind and integrity; runtime build; program identity; the
handler descriptor; the state schema; each argument's schema; for `--fix`, the revision's schema
and handler shape; and the recorded budgets. Each command runs the rows its line of the table
names: a state query checks format, build and state schema, and a failure query also checks each
argument's schema against the entry's trailing parameters, but neither checks the program.

A mismatch is refused with what differs and both values. Schema changes are refused and
migration is Phase 5. A state query needs the producer's runtime build in v0.1 because nothing
yet shows two builds encode identically; it does not need the producer's whole program, so added
query functions are allowed.

## 8. Limits for offline work

`mo query` takes `--max-stack`, `--max-depth` and `--max-heap` as `mo run` does, plus
`--max-work`, defaulting to `MO_LIMIT_QUERY_WORK` (1,073,741,824 instructions per run). Work is
counted through the controlled call the service uses, so a query that runs out stops with a report
naming `--max-work`. `mo history` runs no Mo code and takes none of them; its copy is bounded by
`MO_LIMIT_HISTORY_BYTES` (section 9). Replay uses only the recorded budgets and takes none of these
flags. Each is a named limit in `src/base/limits.h`.

## 9. Reading a stopped data folder

`mo history` and `mo replay --data` use a new reader in `host/store.c`, never `mo_service_open`,
which can run timers and take snapshots.

- It opens `DIR/service.sqlite` read-only, never creates it, and never waits for a lock. It
  classifies the folder by SQLite's result code, never by whether a `-journal` file exists: a
  `-journal` file stays beside the database while a service runs and after a service killed while
  idle, and neither is hot. A running `mo serve` holds the database in exclusive locking mode, so
  the read fails `SQLITE_BUSY` and the command refuses with "a running service owns DIR". A file the
  user cannot read refuses with "cannot read DIR", never as missing or damaged; a missing file is
  "no saved data in DIR". A store without the failure tables predates failure capture and holds no
  failure records; nothing is created. Each refusal exits 1.
- A hot journal (left by a write that did not finish) needs a write to roll back; a read-only read
  then fails `SQLITE_READONLY_ROLLBACK`. The reader refuses with "recovery required" unless given
  `--recover`, which opens read-write as the folder's owner and lets SQLite roll back. That
  rollback writes database pages: the command is logically read-only, since it never changes
  application state or the log, but it is not physically read-only, and it says so.
- It holds the lock only while copying what it needs, in one read transaction: the snapshot, the
  tail's metadata (sequence, handler index, timer flag, time, argument sizes) and the failure
  records. Log rows are streamed so their checksums cover every byte while only metadata is kept.
  Retained bytes are bounded by `MO_LIMIT_HISTORY_BYTES` (64 MiB) in total. Argument values are
  read only for the record being replayed. Over the limit, it reports what it read and what it
  left.
- The snapshot is the state as of its sequence, not the current state: the text says how many
  later messages were not applied. There is no offline current state and no state query over a
  stopped folder; reconstructing the current state means re-running the tail through the handlers,
  which is recovery.

**History depth.** The history trace (`learnability/phase4/preflight/README.md`, "What survives a
snapshot") killed one job with three lease and fail calls and then sent 120 more updates. The
stopped folder held the current state, showing the job dead after three attempts, and a log tail
of sequences 101 to 128 holding only `submit` calls: the calls that killed the job were gone. v0.1
accepts that limit. The evaluation's failure scenarios rely on failure records, which snapshots
never remove, and its wrong-reply scenarios rely on current state. An archive of every committed
message would grow without bound (up to 16 MiB per message) and is not built in this phase.

**Startup contention.** Today a reader holding the database makes `mo serve` fail at once with
E1409 and exit 1, which the task-queue unit lists in `RestartPreventExitStatus`, so a stray
`mo history` could keep a service down. `mo serve` will wait up to `MO_LIMIT_SERVE_STORE_WAIT_US`
(10 seconds) for a lock held by another process, and then exit with 75 (`EX_TEMPFAIL`), a new
status that service managers retry: the task-queue unit's `RestartPreventExitStatus=1 2 69`
does not list it, so `Restart=on-failure` retries. This lands before the reader ships.

## 10. Budgets

| Limit | Value | Charged to |
| --- | --- | --- |
| `MO_LIMIT_EXPORT_PINS` | 2 | — |
| `MO_LIMIT_EXPORT_BYTES` | 2 × (16 MiB + 1 MiB) | native memory, outside the heap |
| `MO_LIMIT_EXPORT_PAGE_BYTES` | 8 KiB | each reply |
| `MO_LIMIT_EXPORT_IDLE_US` | 60 s | — |
| `MO_LIMIT_FETCH_US` | 10 min | the fetch command |
| `MO_LIMIT_FAILURE_CONTEXT_BYTES` | 64 KiB | native, per capture |
| `MO_LIMIT_FAILURE_SCRATCH` | 1 MiB | native, per capture |
| `MO_LIMIT_FAILURE_RECORD_BYTES` | 16 MiB + 1 MiB | per record |
| `MO_LIMIT_FAILURE_RECORDS` | 64 | the store |
| `MO_LIMIT_FAILURE_STORE_BYTES` | 64 MiB | the store's disk |
| `MO_LIMIT_SERVICE_EVENTS` | 256 | native |
| `MO_LIMIT_MCP_VIEW_BYTES` | 6,144 bytes | each rendered view |
| `MO_LIMIT_MCP_VIEW_MEMORY` | 16 MiB + 16 MiB | one operator request's heap |
| `MO_LIMIT_MCP_VIEW_TIMERS` | 256 | timers copied for one page |
| `MO_LIMIT_MCP_VIEW_NAME_BYTES` | 512 | each copied name |
| `MO_LIMIT_HISTORY_BYTES` | 64 MiB | the reader |
| `MO_LIMIT_SERVE_STORE_WAIT_US` | 10 s | startup |

**Process sizing.** The service process is 64 MiB of stack and heap, 8 MiB of metadata and a
24 MiB SQLite budget per store today. Phase 4 adds the encoded current state (up to 16 MiB), pins (up to
34 MiB) and capture buffers (about 1 MiB). The sizing experiment measured today's runtime: an
empty service peaked at 54 MiB resident; the growth fixture's largest accepted state, 16.0 MiB
of text, peaked at 120 MiB resident with a 15.4 MiB snapshot record. The full task queue
at its limits (5,000 jobs of 2,048 bytes) was a 10.0 MiB snapshot record and peaked at 108 MiB
resident. A deployment should allow about 120 MiB per service for today's runtime and 50 MiB more
for Phase 4. With the operator tools, `learnability/phase4/live-views/sizing.py` measures one
16,000,000-byte Text state; its result, with the heap and scratch peaks that `mo-memory` now
reports, is in that folder's `results/sizing.json`.

The completed U9 measurement below includes the fetch process. On the measured 8-vCPU Linux
host, a full 16 MiB saved encoding used 132.7 MiB service RSS and 57.6 MiB fetch RSS at their
separate high-water marks. Retain the service's approximately 170 MiB deployment allowance
above and allow another 64 MiB for the ordinary fetch executable; these are deployment
allowances informed by one fixture, not universal RSS limits. Sanitizer instrumentation is
outside those deployment measurements. **2026-11-07:** U4 capture later landed; the reserved-allowance sentence here was U9 staging, not current status. See [failure capture](../../learnability/phase4/failure-capture/README.md) and Retained diagnostic implementation below.

## 11. Verification

Each property states its prediction before its check runs, gets the Working Agreement's
independent check, and keeps its failures. A check that cannot run (a missing tool) is a skip,
and a skip is not a pass.

### 11.1 Lean

Lean remains the semantic model. The existing theorems prove Core type safety and determinism;
they do not cover lowering, the C runtime, or the service VM's instruction budget, which differs
from Core fuel (`formal/README.md`, `formal/TEXT.md`). The new modules are `MoCore/Saved.lean`
and `MoCore/Verdict.lean`, imported by `formal/MoCore.lean` so that `lake build MoCore`, which
`make formal` runs, checks them.

1. **Reconstruction preserves values.** An executable model of the saved schema and value
   encoding for Core values: `encode : SchemaTy → Val → Option Bytes` and
   `decode : SchemaTy → Bytes → Option Val`. Theorem: for every well-typed `v : ValTy τ`,
   `encode τ v = some b → decode τ b = some v`, and `decode` refuses bytes whose schema differs
   from `schema τ`. A failure's inputs, the tuple `(state, time, args)`, round-trip as a whole.
   The C decoder is not proved: its contract is the hypothesis, and the C unit tests and the
   saved-state fuzz target exercise the same cases (typed, malformed, cross-schema).
2. **The verdict is sound.** `verdict : Outcome → Outcome → Verdict` over the statuses of section
   7.1, against an independently written specification: `verdict o r = verified ↔
   (reproduced o ∧ succeeded r ∧ sizeOk r)`. Corollary: `¬ reproduced o → verdict o r ≠ verified`,
   for every `r`. The matching relation of section 6.2 is modelled and proved as well. The earlier
   corollary that replaying twice gives the same outcome is dropped: the existing
   `eval_deterministic` is `h₁ ▸ h₂` and says nothing about replay. What it was meant to show, that
   replay runs on exactly the recorded inputs, is carried by the round trip of the input tuple in
   item 1.
3. **Negative controls.** A copy of `verdict` that returns `verified` when the revised run
   succeeds alone must fail the soundness theorem (kept as a `#guard` that the false statement
   is refutable by a concrete counterexample), and a decoder that ignores the schema must fail
   the refusal half of the round-trip theorem.

No theorem may be discharged by `sorry`, `native_decide`, an `axiom` or a precondition that
assumes its conclusion. The check is new: `ci/check.sh lean` will run `#print axioms` on each
new theorem and fail on anything beyond `propext`, `Classical.choice` and `Quot.sound`. Today the
repository only claims this; nothing checks it.

### 11.2 TLA+

Two small models, checked by TLC from the TLA+ tools (version pinned by SHA-256 when they land) in
a new `models` group of `ci/check.sh`. TLC checks finite instances; a pass is not a proof of the C.

**Export generations** (`models/Export.tla`). Variables: the service's sequence and state; the
current pin per kind (id, sequence, bytes, released flag); each fetch's accepted id and pages; a
restart counter. Actions: `Commit`, `CreatePin`, `ReadPage`, `Replace`, `Expire`, `Restart`,
`FetchAccept`. Invariants:

- `NoMixedGeneration`: every page a fetch accepted came from one pin id, and their bytes are that
  pin's bytes;
- `NoReleasedRead`: no page is served from a released pin.

Configuration: two kinds collapsed to one, three sequence values, two restarts, pins with the same
sequence allowed. Negative control: a variant whose `ReadPage` keys pins by sequence instead of id
must violate `NoMixedGeneration`. Progress (`a fetch with no replacement and no expiry completes`)
is checked only under weak fairness of `ReadPage` and with `Replace` and `Expire` disabled.

**Failure capture** (`models/Capture.tla`). Variables: application state and log position, with
their values at the failure; the retained records (id, kind, first flag) and their two halves;
published ids; supervisor count; store health; a pending capture's phase, rows and deletions.
Actions: `Apply`, `Snapshot`, `Fail`, `Prepare`, `PrepareRefuse`, `Retain`, `Evict`,
`RetainRefuse`, `Write`, `Commit`, `CommitUncertain`, `Rollback`, `RollbackFail`, `Crash`,
`Recover`, `Count`, `Reopen`. `Apply` and `Snapshot` are the successful handlers that change
application state and the log, so `RecoveryUntouched` constrains the capture actions.
Invariants:

- `PairComplete`: every published or retained record id has both halves;
- `RecoveryUntouched`: from `Fail` until the service is idle again, application state and log
  position keep their values at the failure;
- `CountedOnce`: each failure increments the supervisor count exactly once, whatever the capture
  outcome;
- `NoUnsafeContinue`: after `RollbackFail` or `CommitUncertain` the service is stopped until
  `Reopen`;
- `Retention`: at most the record cap, and each kind holds one first record and at most two;
- `RepeatKeepsOthers`: a repeat of a kind never deletes another kind's record.

SQLite's atomic commit is modelled as the `Commit` action, with `CommitUncertain` (the
transaction may or may not be durable, and no id is published) and `Crash` between `Write` and
`Count` as the failures it must survive; the model does not assume the property it checks. The
byte cap is not modelled. Each invariant has a negative control that must violate it: publishing
the id at `Write`, counting the failure in `Rollback` as well, advancing the log in `Commit`,
continuing after an uncertain outcome, inserting a new kind without eviction, and evicting
another kind for a repeat (`models/README.md`). Each counterexample found while writing the
models becomes a store or process test.

### 11.3 C

| Property | Evidence |
| --- | --- |
| The page reader and fetch bounds | `fuzz_fetch`; a CBMC harness over the page-metadata checks (offset, length, total and coverage arithmetic) with unwinding and reach checks |
| Failure record framing | the existing envelope reader gains kinds 3 and 4; `fuzz_service_failure` (a third wrapper of `service_fuzz.h`); a CBMC harness for the message payload's length arithmetic |
| Offline artifact reader | `fuzz_artifact`; CBMC over the `MOEX` header reader |
| SHA-256 | FIPS 180-4 vectors, a long-message vector and a differential unit test |
| Capture under faults | allocation fault injection by compile-time renaming in the capture translation unit; a fake store returning each SQLite failure class of section 6.4; process kill tests at each step of section 6.3 |
| Exact budgets | the work-limit capture at exactly `MO_LIMIT_SERVICE_WORK`; the live-mailbox regression: a handler that fails with a full mailbox and succeeds offline is `not_reproduced`, and its unchanged handler is never `verified` |

### 11.4 Safe-c

Not pursued in this design step. Its practices (exact postconditions, independent property
clients, negative controls, a mapping from model actions to C) are used above. No library is
adopted.

## 12. Decisions made at approval

Robert accepted each of these on 2026-11-04, as written here.

1. **The runtime build identity is the SHA-256 of the `mo` executable** (section 4), not a digest
   of chosen build inputs. A list of inputs that misses one would let two different runtimes
   claim to be the same, and a replay verdict would then be wrong without saying so; an exact
   identity only ever asks for the original executable, which deployments already keep.
2. **The runtime keeps the current state's encoding**, up to 16 MiB of native memory per service
   (section 6.3), rather than encoding at failure time, because encoding needs heap scratch and
   the failures most worth recording are heap exhaustions.
3. **`mo serve` waits up to 10 seconds for a store lock another process holds, then exits 75**
   (section 9), a status service managers retry, so a reader at the wrong moment cannot keep a
   service down.
4. **Retention is 64 records and 64 MiB, a first and a latest record per kind** (section 6.5), so
   a repeating failure rewrites one row and never evicts another kind's first failure.
5. **History is the log since the last snapshot** (section 9). An archive of every message waits
   for evidence that agents need it; failure records already survive snapshots.
6. **Evaluation sessions run on the service's own machine** (section 2). A proxy-only remote
   operator gets the views alone, and that stays a documented limit of v0.1.

## Approval and changes

| Date | What | Who | Evidence |
| --- | --- | --- | --- |
| 2026-11-04 | First proposal | Claude (design step) | this file's first commit, PR #288 |
| 2026-11-04 | Approved, with the six decisions in section 12 as proposed | Robert | his "accept all" in the design-step session, after each decision was walked through; this row's commit |

| 2026-11-05 | Replace the mandatory old baseline prerequisite with pilot findings and prospective diagnostic evaluation; technical contracts remain in force | Robert’s direction, recorded by Codex | canonical phase plan’s dated amendment and `learnability/phase4/pilot/findings.md` |
| 2026-11-06 | §7.3: totals keep `status`; JSON values use the saved-schema renderer; a query whose function stops exits 70; history tail entries carry the handler index, not a name | Robert, approving the offline diagnosis plan's scoping | `docs/plans/2026-11-06-1220-feat-phase4-offline-diagnosis-plan.md` |
| 2026-11-06 | §7.4 and §9: history takes no program and shows per-record build identity; checks run in a fixed order per command; the reader classifies by SQLite result code, never waits, and names unreadable, missing and pre-capture stores | Robert, approving the offline diagnosis plan's scoping | the same plan |
| 2026-11-06 | §4: pre-Phase-4 state is read by history, state queries need a Phase 4 export, and records from builds without `mo replay` are evidence only | Robert, approving the offline diagnosis plan's scoping | the same plan |
| 2026-11-06 | §7.1: budgets are compared with the build's constants; both programs are checked before anything runs; timers get the size check; an `Error` reply is a success reported by kind | Robert, approving the offline diagnosis plan's scoping | the same plan |
| 2026-11-06 | §7.2: the state type is that of FILE's main service; generic entries are refused; entries are selectable whether exposed or not; the capture clause is dropped | Robert, approving the offline diagnosis plan's scoping | the same plan |
| 2026-11-06 | §8: `--max-work` counts through the controlled call, default `MO_LIMIT_QUERY_WORK`; history takes no Mo limits | Robert, approving the offline diagnosis plan's scoping | the same plan |
| 2026-11-06 | §11.1: the replay-twice corollary is dropped; the input tuple's round trip carries its intent | Robert, approving the offline diagnosis plan's scoping | the same plan |

### Implementation staging and observation metadata

**2026-11-07:** The notes in this subsection and the U8/U9 evidence below describe those merges. U4 capture and failure-kind export later landed; see Retained diagnostic implementation and §5.1 (`kind: "failure"`). They are not a current unavailability claim.

The operator milestone first retains the current saved encoding and bounded native metadata, then exposes authenticated live views, then state export and private fetch. Failure-backed catalog entries and failure-kind export become available with U4’s capture/retention implementation. Earlier milestones omit unavailable capabilities from discovery and refuse direct requests through normal unsupported-action validation; they never return an ordinary empty success suggesting that capture is functioning. Views that reference a later capability must state its current availability.

Mailbox argument byte size is the encoded length when available. For legal read-only arguments whose encoding cannot be obtained within the diagnostic bounds, return an explicit unavailable value and reason. Do not reject an otherwise legal call to satisfy a diagnostic size field, report a fabricated zero, or retain live argument values beyond their existing lifetime.

The cached current-state encoding is an U3 foundation dependency even though its design is described in §6.3. Implementers must preserve initialization, recovery, failed-append and snapshot-failure behavior. Non-persistent service loops must not acquire new saved-codec admission requirements solely to support inspection; accessors report unavailable encoded state when that service mode cannot supply it. The operator/fetch workflow is verified on the served persistent service path.

### Native observation foundation

The native `vm.h` observation API is idle-only. It copies bounded scalar metadata and
borrows the current encoded state until the next step or destruction; it allocates no
scratch and retains no live values. A full mailbox is observable without admission.
Supervisor timestamps are the retained accounting windows at `observed_elapsed`, the
last clock sample, rather than a fresh clock read. Heap use/peak and stack capacity are
reported separately. The fixed event ring retains numbers, kind, reason, time and handler
function index, never transient defect pointers.

Persistent loops retain the validated saved encoding in an independently owned buffer
bounded by `MO_LIMIT_SAVED_BYTES`, plus one arena chunk header. The previous cache and
candidate encoding coexist during update preparation; publication moves the candidate
buffer and frees the previous cache. Existing codec scratch remains charged to the heap.
A failed append or snapshot leaves the live state and cache unchanged. A successful append
followed by snapshot failure advances the recovery sequence but stops the loop; reopening
replays that committed update and obtains its encoding. The separate encoded-state sequence
identifies the cached bytes even when they trail the recovery sequence. Snapshot sequence changes only
when a snapshot succeeds. Nonpersistent loops do not acquire codec requirements and
report encoded state unavailable.

Mailbox argument sizes sum the saved argument payload lengths, excluding log framing.
They are available for persistent updating calls and zero-argument calls. Otherwise the
availability flag is false: the arguments were not encoded at admission. Observing them
never encodes or retains additional values. Timers have zero argument bytes.

The executable initializes runtime identity once before command execution, except for
`mo check`, which does not consume runtime identity. Linux reads `/proc/self/exe`;
macOS resolves the executable path. A failed read leaves identity unavailable without
changing ordinary command behavior. Consumers must check availability. The build label
includes Mo's version, compiler version and build configuration. `ci/check_sha256.py`
compares the native digest against Python's SHA-256 for the executing test binary and
110 deterministic boundary/random inputs; `test_sha256` also pins standard digest vectors.
No authenticated operator entry point, artifact format or fetch command is introduced by
these native interfaces.

The debug build optimizes the SHA-256 arithmetic kernel while retaining debug settings
for the rest of the toolchain. `mo check` skips executable hashing to preserve its
0.1-second sample latency budget; commands that consume identity still hash the measured
executable bytes before execution.
The service-store fault harness separately refuses the recovery cache copy, checking a
memory refusal, release of the decoded state and successful reopening afterward.

### Authenticated live views

The supported catalog now includes overview, state, mailbox, timers, memory, supervisor and
post-snapshot log views. Authentication precedes validated operator dispatch; operator calls
consume the same rate allowance as author calls before any inspection or author conversion.
They never enter service admission. State export is now available; retained-failure tools remain absent.

`runtime/mcp-views.mo` renders the saved schema and payload without extending bytecode shape
reachability. The native bridge copies typed metadata and saved bytes into the request process;
no live service value is retained. Its extra saved-byte allowance is independently bounded by
`MO_LIMIT_MCP_VIEW_MEMORY` (the ordinary 16 MiB request allowance plus 16 MiB of saved bytes).
Native timer metadata is copied one page of at most `MO_LIMIT_MCP_VIEW_TIMERS` (256) at a time,
from the `offset` the transport parses into the operator action, and the omitted count includes
timers outside that page. Names beyond `MO_LIMIT_MCP_VIEW_NAME_BYTES` (512) are
explicitly unavailable rather than copied without bound.
Only a rate-admitted operator request gets that allowance; it is restored after rendering and
the copied bytes are released before another service step. The renderer's JSON
budget is `MO_LIMIT_MCP_VIEW_BYTES` (6144 bytes), an upper bound on the rendered state that
counts separators and leaves room for MCP's duplicated textual and structured result. Truncation
follows section 3.3. A narrower path remains usable; export advice points to `mo-export`. Overview uptime is the server's monotonic clock less its start, so it keeps
advancing while the service is stopped.
The memory view reports the high-water native scratch of completed inspection rendering calls,
including their protocol work, rather than claiming an atomic measurement of the current call.

The service keeps at most one snapshot interval of scalar log metadata. Existing validated
recovery and successful append populate it; successful snapshot clears it. Failed append leaves
it unchanged, while successful append followed by failed snapshot keeps the committed entry,
matching the durable sequence rather than the older encoded-state sequence. No new store reader
or record format is introduced.

Protocol tests exercise private nominal state, scalar author-tool name collisions, full-mailbox
inspection, shared rate limits and non-admission. The operator driver exercises actual loopback
HTTP, saved scalar/container forms, path selection, explicit truncation and token refusals.
The saved renderer has a registered fuzz target; the bytecode/shape ABI is unchanged and the
embedded runtime ABI rejects reordered observation fields. These are implementation checks,
not scored diagnostic evaluation observations.


### Immutable state export producer

U8 added `mo-export` for state only. At that merge, failure-kind export remained unavailable until U4;
`mo fetch` and final-file publication remained U9. **2026-11-07:** U4 and U9 later landed (Retained diagnostic implementation). JSON shaping, argument validation and
base64 are embedded Mo; the native bridge copies cached MOSV bytes and serves bounded
pages without encoding live values or altering bytecode shape tables.

MOEX v1 has 162 fixed bytes including its trailing digest, plus the UTF-8 build label
and exact MOSV body. Both sequence fields are unsigned little-endian 64-bit values.
The UTC creation timestamp is signed 64-bit microseconds. The encoded-state sequence
identifies the body; the durable sequence identifies the observed recovery head. In the
snapshot-failure regression these are respectively 99 and 100, and recovery later
produces encoding 100 without changing the earlier artifact. Schema SHA-256 covers
exactly the schema span identified by MOSV's four-byte length, excluding magic, length
and value bytes. The artifact digest covers the entire frame preceding its final 32 bytes.

Start with `{"kind":"state"}`. Continuations require `kind`, a lowercase 32-hex-digit
`export`, and an aligned `offset` (default zero). Unknown fields, failure kind, malformed
IDs, negative/fractional/unaligned offsets, offsets at the aggregate bound and nonzero
start offsets are invalid params. An aligned offset beyond the actual pin is an
`invalid_offset` tool result. Page metadata uses decimal strings for both sequences;
`offset`, `length`, `total` and `pages` are JSON integers. `checksum` is 16 lowercase hex
digits of FNV-1a-64; `digest` is null except on the final page, where it is SHA-256 hex.
`next` is the next call's arguments or null. Requests may reread aligned pages while
the pin lives; reading its last page releases it even if earlier pages were skipped.

Authentication and the shared rate gate precede native export work. Refusals have an
`error` and `state_effect: "none"`; unavailable state/build/randomness is explicit.
Export refusals set MCP `isError: true`; successful pages set it to false. Their
structured payload remains under `value`, without classifying an operator refusal as
an author application error. A successful new ID is drawn from OS entropy, checked against the current pin, and
independent of sequences. Random uniqueness is probabilistic, not a mathematical
cross-process uniqueness or authenticity guarantee. An absent pin yields
`export_released`; a mismatched ID with a live pin yields `export_replaced`.

State owns one confidential arena, bounded by half `MO_LIMIT_EXPORT_BYTES` including
its chunk header. Replacement frees it before allocating another, so an allocation
failure can release the old pin but cannot double its resident allowance. Expiry is
checked during polling and before every export; its deadline participates in polling.
The second kind's allowance was reserved for U4 at this merge and was not then allocated. **2026-11-07:** U4 later allocated it (Retained diagnostic implementation). Copying and hashing
use constant stack space beyond the pin. One page and its rendering stay inside
`MO_LIMIT_EXPORT_SCRATCH` (the existing 16 MiB per-request budget), independent of the
service heap, and no Mo reference survives the request. Pin bytes reported by
`mo-memory` include allocator overhead. Full process sizing with the fetcher is recorded in the private-fetch evidence below.

Verification: `tests/test_export.c` covers literal page boundaries and a full 16 MiB
MOSV copy; `test_protocol_server` covers generation traces, full mailbox and rate
refusals; `test_protocol_memory` covers allocation failure, confidential release and
pin creation with an exhausted live heap. `test_service_store` pins snapshot-failure
provenance and existing recovery behavior. `ci/check_export.py` independently parses
actual HTTP pages and verifies framing, every identity and digest, old bytes across
commits, replacement, malformed arguments, auth and nonmutation. It tests the producer
independently of the fetch command. `models/README.md` records finite TLC assumptions and
negative controls; `cbmc/README.md` records bounded reader/arithmetic proof scope.
`fuzz_export` exercises malformed MOSV schema spans and page arithmetic with exact
acceptance oracles; the existing protocol-server fuzzer covers the embedded request path.

Development observations: the new native test initially failed because the export API
did not exist. GCC then rejected discarded must-use expiry results; embedded Mo rejected
catch-all choice arms. Both were corrected. The initial Python fixture used unsupported
text concatenation/size spellings and was corrected to interpolation/byte_count; its
independent framing checks then passed. The TLA release control's initial specification
error and correction are recorded with the model. A compiling C mutant bypassing the
ID comparison failed both stale-generation assertions; the restored producer passed.

### Private fetch implementation evidence

The host-local command now creates a new state generation on each invocation, verifies exact
page coverage, generation and both sequence fields, validates the complete MOEX framing and
schema/artifact digests, and publishes through a non-overwriting hard link. Pin creation,
final-page completion/release and local file publication remain distinct lifecycle events.
At this U9 write-up, failure-kind export/capture remained unavailable. **2026-11-07:** both later landed; see Retained diagnostic implementation.

Development checks have exercised a 14-page private nominal state export with proxies configured,
mode-0600 publication, existing destinations and symlinks, wrong credentials, malformed/truncated/
oversized responses, redirects, corrupt metadata/checksums/base64 and signal cancellation. An
independent socket relay also fetched a three-page 20,000-byte state during a commit: the first
artifact contained exactly the prior state and the retry used a different generation containing
the next committed state. This is development acceptance, not scored diagnostic evaluation.

Three earlier authoritative check runs were intentionally stopped before further edits;
their files remain `build/u9-check-results-{1,2,3}.txt`. They are interrupted runs, not passes.
One observed failure was confidential response-buffer growth exhausting its allowance at 16 KiB;
the fetcher now allocates the bounded response buffer once. The independent simplification pass
also found undefined subtraction between identical string-literal occurrences, wire-type
coercion, exact-umask permissions and publication/cleanup reporting issues, which were corrected.

Final GCC, Clang and ASan/UBSan suites passed. Static checks and the pinned TLC model group
passed without skips, including expected generation/release counterexamples. All-build drivers
passed against GCC debug, Clang debug and Clang sanitizer, including CLI goldens/guide claims,
operator views, producer exports, fetch, and formatter behavior. All 12 CLI timing samples
remained below 0.1 seconds (largest best-of-five observation: 0.0983 seconds). A subsequent
all-build fetch rerun passed stronger direct program/build/schema/sequence and exact-state
assertions during commits, plus positive controls for all three supported loopback URL forms.
Real idle expiry followed by a fresh-generation retry also passed.

The stateful reader fuzz smoke completed 44 runs in 16 seconds under ASan/UBSan, with valid
multi-page and changed-generation/sequence/coverage seeds; CLI fuzz completed 32,020 runs in
11 seconds. These are bounded empirical runs. CBMC proved and reached every block of
`export_page_matches`, `mo_export_page_bounds` and `mo_export_schema`. The new admission proof
uses unconstrained metadata and 16-byte IDs with 17 unwinds; it covers generation, both
sequences, count and coverage arithmetic, not crypto, transport, copying or file syscalls.

The maximum fixture independently verified a 16,777,216-byte MOSV encoding in a 16,777,405-byte
artifact across 2,049 pages, published mode 0600 in 532.07 seconds. The expected private nominal
state uses the specified Choice constructor header, answer 42 and 16,767,505 repeated Text
bytes. Service RSS high-water was 135,928 KiB; fetch RSS high-water was 58,972 KiB. Pin storage
was 16,777,437 bytes including allocator overhead and zero after completion. Reported diagnostic
scratch peaked at 88,222 bytes, independently of the pin and service heap. One pending socket
requests a 33,792-byte receive buffer (this Linux host reports 67,584 bytes), beside one bounded
wire buffer; fetch parser heap is limited to 16 MiB. Process measurements include parser and
compiler costs. See `learnability/phase4/private-fetch/results/sizing-maximum.json`.

Three serial attempts timed out near 600 seconds. Three later lookahead fetches exited 0, but
their measurement oracle misread the nominal struct header; those harness runs are failures.
The corrected oracle follows the saved-state specification and lowering, passed first on a
small fixture, then passed the maximum fixture above. Failed logs and predictions are retained.

The checked-in native-client smoke passed with Codex 0.166.0, GPT-6.1 Sol/high, explicit tool
approval and shell network access. It discovered and called overview/export, ran the provided
fetch command, then read mode 0600 and MOEX magic locally. This establishes prompted workflow
support, not effectiveness. Pilot observations and prospective U6 gates are unchanged.

Independent simplification and a completed ten-perspective local review found six issues;
their fixes and verification are reconciled in
`learnability/phase4/private-fetch/results/verification.md`. A fresh focused read-only follow-up
found no new concrete defect in lookahead/deadline changes. Cross-provider review did not run
because its tool lifecycle was incompatible; no cross-provider result is claimed.

The deadline now starts at command entry and is armed before the first destination lookup.
Numeric-only resolution avoids DNS/NSS; a monotonic POSIX timer interrupts blocking POSIX calls,
including destination lookup and filesystem completion. Deadline checks surround publication and
remove a late link. As with the runtime's other wall budgets, delivery/cleanup assumes the OS can
schedule the process and interrupt a blocking syscall; uninterruptible kernel waits are outside
that guarantee. Compiled two-second controls interrupt five-second lookup and fsync stalls;
SIGTERM injected immediately after temporary creation verifies handler ordering and cleanup.

## Retained diagnostic implementation

The runtime captures from live frames before normal cleanup and uses a dedicated
native-only saved encoder for arguments. This refines section 6.3: no encoding
scratch is charged to an exhausted handler heap, and no mailbox retains a second
copy of potentially 64 times 16 MiB of admitted arguments. Existing synchronous
defect hooks still see valid live frames. Normal cleanup runs once after reporting
and reply; no frame survives into another execution. The encoder shares the saved
schema/value walkers, bounds its native schema scratch to 1 MiB, and rolls back
partial output on refusal. Exact-limit admission remains unchanged.

Message metadata is at most 1 MiB. A named 512-byte reserve within the combined
17 MiB capture budget covers both MOST envelopes, pair lengths, and the maximum
MOEX header/label, so every captured pair can fit its export pin. Context walks at
most eight frames and visits at most 32 variables, with a separate one-million-unit
inspection budget. Each variable line states `available` with the value or
`unavailable` without it; a rendering longer than `MO_LIMIT_FAILURE_VALUE_BYTES`
(1 KiB) is `unavailable`, not truncated. Top-level values are quoted and escaped
exactly as nested values are, so the context never holds a raw NUL or newline taken
from a value.

The version-1 message payload uses little-endian integers: one-byte version,
8-byte id/sequence/arrival, one-byte timer, 4-byte handler, length-prefixed UTF-8
name, one-byte read-only flag, 4-byte count and length-prefixed MOSV arguments;
one-byte signature version/class, length-prefixed reason/limit and its 8-byte exact value, four 4-byte
location fields, five 8-byte budgets, the 8-byte store image key, 32-byte build digest,
length-prefixed label, 32-byte schema digest, one-byte context status and
length-prefixed context text. The image key is the value every `MOST` frame header
carries: the program checksum XOR the service index shifted left 32 bits
(`mo_image_key`). A failure export XORs the service index back out, so its producer
program field is the plain program checksum, as in a state export. Class values are
defect=0, work=1, memory=2, state-size=3; context status is complete=0, truncated=1,
unavailable=2. Record id is bound inside the message before the same transaction
commits both rows. Before storing, the runtime decodes the message it built with
`mo_failure_read`; a message that does not decode is refused (section 6.4) and
nothing is written.

Failure exports allocate their final confidential pin directly from borrowed
SQLite payloads, without an intermediate complete pair buffer. State and failure
pins remain independent and refresh only after a successful page. A request for a
failure id that is not retained is refused and leaves the current failure export
in place; only a successful new failure pin replaces it. Evicted-kind counts
describe the current open handle, not durable history. Repeated-kind growth that
cannot fit without evicting another kind is explicitly refused (section 6.5).

Damaged diagnostic rows (a record missing one of its rows, a kind of the wrong
length or not UTF-8, a `first` flag other than 0 or 1, more than 64 pairs) make a
new capture a clean refusal, so they no longer stop the service. Listing and
exporting those rows still report `corrupt`.

The retained-failure catalog reads at most eight message records after its cursor and only
state lengths, charging message bytes to the request work budget. It reports the exact
remaining record count. A stored message that does not decode is skipped, and the rest of
the page still renders. Pin creation performs an indexed id lookup and validates that one
complete pair; it does not checksum unrelated retained state blobs.

Development evidence and limitations are recorded in
[the failure capture results](../../learnability/phase4/failure-capture/README.md).

## Offline commands implementation evidence

Predictions for each check are in `learnability/phase4/offline/results/predictions.md`, written
before the checks ran. Results, measurements and failed attempts are added here and in that folder
as the offline commands land.

**Offline value rendering.** JSON values in `mo replay`, `mo query` and `mo history` come from
`McpViews.offline_value`, the live views' saved-schema renderer without a path, with a budget of
`MO_LIMIT_OFFLINE_VALUE_BYTES` (16 MiB) counted as the live views count it. The budget is sized
from the task queue's largest state: 5,000 jobs with 2,048-byte payloads (100 of them leased),
10,461,125 saved bytes, rendered whole (`complete: true`) by `mo history --json` to 10,922,848
bytes of JSON, with a peak resident size of 144 MB against the renderer's
`MO_LIMIT_OFFLINE_RENDER_MEMORY` heap of 256 MiB. That render took 220 seconds in a `-O0` debug
build; rendering speed is not yet tuned. Text output for `mo replay` and `mo query` prints values
with `mo_value_print` in the program's own types; `mo history` has no program, so its text form
shows the same JSON rendering.

**End to end, the grader, and the reader's budgets.** `ci/check_offline.py` follows the agent
guide's offline how-to against a real service whose read-only tool, updating tool and timer each
failed: each replays to `reproduced` from the stopped folder and from its fetched artifact,
`mo history` matches an independent read-only SQLite reader of the log and the live
`mo-failures` records, it refuses the folder at once while the service runs, and a reader holding
the store for 3 seconds delays `mo serve` without making it exit 75. The kill loop runs
`mo history` between every kill and restart and accepts only a read or "recovery required". On
the synthetic variant, `learnability/phase4/test_offline.py` shows the fix check's one-way
relation with the grader: the unchanged program is `not_fixed`, the reference fix `verified` and
passing, and the broken fix `verified` but failing the hidden tests, the expected gap between "the
failure is gone" and "the answer is right". The queued-mailbox memory failure stays proved at the
VM level only (`offline_memory` in `tests/test_service_store.c`); a real service cannot fill its
heap through the request limits. On the largest task-queue folder the reader holds its lock for
0.05 s (`-O2`) to 0.07 s (`-O0`) with a 144 MB peak, and for 0.01 s on a tail of 99 near-limit
rows. Rendering is linear in the state's size, about 8 s per MB at `-O2` and 20 s per MB at
`-O0`, all of it the interpreter running `McpViews.offline_value`; the lock is released before
rendering, so `mo serve` started beside a `mo history` of the 10.4 MB state listened within 2.3 s.
The numbers and the commands are in `learnability/phase4/offline/results/verification.md`.

## Proposed whole-program upgrades

[The upgrade design](upgrades.md) specifies the next phase; it is not implemented
or approved. Current behavior above remains the shipped contract. Activation
will atomically record the source bundle, migrated snapshot and program identities,
with a version-2 folder that old readers refuse. The new upgrade operator tools
change the service and require a separate credential. A compiler child limits
crashes and stalls but shares the host identity, files and secrets: its compiler
is part of the upgrade credential’s trusted base. Write access to the future
program-owning data folder confers the authority to replace the running program.
