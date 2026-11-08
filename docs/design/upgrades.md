# Whole-program upgrades

Status: approved in M1 (PR #310). M2 implements typed checking, migration
validation and checked restart; M3 offline rehearsal; M4 live swapping at a
message boundary, the upload tools, MCP streams and `mo push` (see Live
implementation below). The operator-catalog polish and the remote procedure
remain M5.
The feasibility evidence, including failed assumptions and corrected results, is
in [the preflight](../../learnability/phase5/preflight/README.md).

## Authority and lifecycle

An upgrade replaces the whole program at a service message boundary. Modules
remain directly called; there is no dynamic call indirection. The candidate is
source compiled by the serving build, not portable bytecode. The live service
continues until a single activation transaction commits. A refusal preserves the
old program, state, mailbox and timer phases. After commit the recovery snapshot
is authoritative; restart never reruns the migration. Later defects require a
forward fix, not automatic rollback.

The host command is `mo push FILE --url URL --base DIGEST --token-env NAME
--upgrade-token-env NAME [--migrate Module.function]`. Its URL must be loopback,
checked before either environment variable is read. Remote operators use a
loopback tunnel; this phase does not add a TLS client. `mo serve --data DIR`
runs the recorded source. `mo serve FILE --data DIR` records the first program
in an empty folder; a different source in an existing folder requires
`--upgrade [--migrate Module.function]`. A checked restart has a larger migration
budget but the same validation and transaction rules.

The explicit legacy-adoption flag is `--adopt`. With no program row, an exact
image-key match permits adoption; otherwise adoption requires that flag and
verified replay. A matching recorded source digest with a new image key also
requires verified replay, without the legacy flag. Every state checksum in the
tail must match; an empty tail checks no handler behavior and the report says so.
A source which no longer compiles is refused with its recording build digest and
first diagnostic; a deployment must not restart-loop on that refusal.

## Candidate bytes and upload

The version-one source bundle is binary: `MOSB`, byte version 1, a little-endian
u32 entry-path byte length and UTF-8 entry path, a u32 file count, then files in
strict ascending UTF-8 path-byte order. Each file is a u32 path length, path,
u32 source length and exact UTF-8 source bytes. No trailing bytes. The source
digest is SHA-256 of that complete canonical encoding; neither timestamps nor
host paths participate. Paths use `/`, are relative, contain no empty, `.` or
`..` components, and cannot shadow `lib/` or the reserved `UpgradeOld` namespace.
Duplicates are refused. Imports resolve only inside the bundle or the build's
standard library. The entry must name one included file.

Proposed bounds: 64 files, 255 path bytes, 1 MiB total bundle, 8 KiB decoded
pages. These are admission limits, not claims that every bundle within them
compiles. Compiler limits still apply. The reader checks every addition before
allocation; readers get CBMC reachability controls and fuzz seeds.

`mo-upgrade-open` takes base source digest, candidate digest, total bytes and
optional migration name. It returns an opaque slot id. `mo-upgrade-page` takes
slot, zero-based index, offset, total and base64 bytes. Each page must be the
next exact range; the last page verifies the digest and starts compilation.
`mo-upgrade-commit` takes the slot and base digest. One slot exists per service;
it expires after 60 seconds without progress. A second open is busy. A digest
already active returns `already active`; a stale base reports the active digest.
Base is checked again at commit. These rules make a lost activation reply
recoverable without reapplying a migration.

Both bearer values are required for every upload/commit call. The additional
header is `Mo-Upgrade-Authorization: Bearer VALUE`; values are 16–256 bytes and
must differ under constant-time comparison. `--upgrade-token-env` enables this
credential on `serve`; absence disables all three tools. Header buffers are
wiped before Mo values or events are built. The current 1 KiB header cap can
hold two maximum credentials plus ordinary MCP headers, but exact-limit wire
fixtures must pin the supported envelope. Client-supplied optional headers can
still exceed that cap and be refused. Proxy redaction covers both headers.

## Compilation and migration

Run the same executable as a child, with a private bundle directory, 1 GiB
address-space bound, 10 CPU seconds and a 15-second wall deadline. Poll it
without blocking the service; kill and reap an expired child. Compile before
requesting a boundary. ASan test builds omit only the virtual-address bound because sanitizer shadow
reservations exceed it; production retains that bound, and all builds retain
CPU and wall limits. The child shares the host identity and is not an OS
sandbox. The verifier checks its output, but does not protect host secrets
from a compiler memory-safety exploit. Long front-end fuzz runs are required.
The child probe also compiles the real service to bytecode text and emits a
generic program to binary. The service-only image transport and verifier
integration remain an implementation obligation.

A migration is a non-generic pure function of exactly one old state parameter
and the new state result. No function or capability may occur inside either
type. Selection is by supplied name and exact old input type. A matching saved
schema digest needs no migration; field names, constructor names and ordering
are part of that digest. Changed access clauses are refused. Tool signatures,
timer names/intervals and state schemas are compared across whole descriptors.

`UpgradeOld` is injected in memory, never read from a candidate's disk path.
Its declarations come first from the recorded source bundle; schema synthesis
is the fallback when that bundle cannot compile. Concrete image type tables
alone do not preserve generic declarations. The decoder receives an
index-to-saved-name override table built by tools. Only nominal names are
substituted; constructor/field order, arity and every nested shape still match
byte for byte. Heap code does not learn module semantics. Unchanged nested
nominal types should retain their original identity where possible.

The preflight extracts old concrete declarations and automatically synthesizes
the task queue from its emitted schema. Recompilation preserves that schema
exactly; adding a field changes it, and the synthesized migration compiles and
runs. Existing entry-check machinery refuses wrong-first-type and capability
controls. Production recursive/generic synthesis, name-index overrides and exact
new-result checks still require implementation and tests.

## Atomic activation and identities

Add strict tables without changing existing table SQL: `program` holds the
active canonical bundle and identities; `upgrades` holds an immutable activation
record keyed by a strictly increasing ordinal; `previous_programs` and
`previous_snapshots` retain the old bundle and encoded snapshot by ordinal.
The new typed host operation performs, in one SQLite transaction: new snapshot
at the current head, tail deletion, program replacement, upgrade insertion,
old bundle/snapshot retention and eviction. An uncertain commit stops the
service as a store failure; serving the old image afterward is forbidden.
Ordinary snapshots never alter upgrade tables. Head message sequence continues.

| Object | Identity and resolution |
| --- | --- |
| Active program | Source SHA-256, image key, build digest, saved schema digest, activation ordinal |
| Upgrade record | Ordinal, head, old/new source digests, old/new image keys, old/new schemas, build digest, migration name, report |
| Retained snapshot | Activation ordinal, original image key and exact encoded state at that head |
| Retained bundle | Source digest and activation/build identity; referenced by ordinal |
| Failure record | Original build, image key and schema; resolve only a matching retained program |
| Live event | Epoch plus original handler index; interpret through that epoch's descriptor |

Retain two previous bundles plus every bundle pinned by a retained failure.
Retain two previous snapshots, at most 32 MiB combined; if the new retention set
cannot fit, refuse before commit. Pin counts inherit the bounded failure store.
For equal image keys, first filter by build and schema, then choose the greatest
activation ordinal no later than the failure's capture when that attribution is
available. Legacy records without an epoch must report ambiguity if distinct
source digests remain; never silently label one as the original source.
Evicted programs render as `program not retained` in history.

Use SQLite `user_version=2` when adopting/recording a program, in the same
transaction. Today's reader rejects versions above 1 but otherwise does not
know program/upgrade tables; version 1 would allow an old binary to open a
folder under an accidentally matching image key and write without maintaining
the active-program contract. Upgrade-capable readers accept legacy version 1
only through the adoption path. Ordinary snapshot writes must preserve 2.

## Durable compilation context

Approved implementation clarification, 2026-11-08: each active and retained
program also carries a bounded, validated compilation context. It is separate
from the canonical original MOSB source bundle and its source SHA-256. A source
can keep a migration body across later tool-only upgrades; reconstructing that
body's old declarations from the latest snapshot loses its original input type.
The context preserves the generated declarations and selected migration root
without requiring a chain of older snapshots or bundles.

MOCC version 1 consists of `MOCC` followed by byte 1, a little-endian u32 length
and UTF-8 migration name, a u32 file count, then length-prefixed path/source pairs
in strictly increasing path-byte order. The migration name is at most 255 bytes.
There are at most 64 files, each path at most 255 bytes, and the entire context
is at most 1 MiB. Paths are `UpgradeOld.mo` or dotted generated module names
under `UpgradeOld`, ending in `.mo`; no separators, NUL, empty components or
traversal are accepted. Source text is UTF-8 without NUL. The canonical empty
context has no migration name and no files. The child compiler emits the context
alongside its verified image; the parent validates and independently hashes it.
A nonempty metadata suffix is a u32 byte length followed by `MOCS` and byte 1:
length-prefixed token environment name, raw saved-state schema, a u32 count and
sorted generated current-state declaration path/source pairs, then sorted tool
and timer summaries. Each summary has a length-prefixed original handler name;
a tool has a 32-byte descriptor digest, and a timer has a positive signed-64-bit
interval encoded as little-endian u64. Counts obey the ordinary handler/file
limits, and the entire context still fits 1 MiB. The store verifies that the raw
schema hashes to the program schema digest. Metadata has no executable code.

The current-state declarations cache reachable generic templates and recursive
nominal types separately from the historical migration declarations. If recorded
source no longer compiles, an empty log tail permits migration from the retained
snapshot using these declarations. If those declarations also fail, concrete
saved-schema synthesis is attempted. Concrete generic instances use reversible
`Saved_<hex-qualified-name>` aliases; shape checking still compares every field,
constructor and type parameter, with explicit nominal-name overrides. Source
structs have single-case choice layout; legacy record layouts that source cannot
reproduce are refused. A nonempty tail that needs an unbuildable recorded handler
is refused without writing. This fallback never runs the new initial state in
place of the saved state or invents replay checksums.

`program` and `previous_programs` add `context_digest BLOB NOT NULL` (32-byte
SHA-256) and `context BLOB NOT NULL`. Context identity participates in program
comparison and is validated before compilation. An ordinary tool-only upgrade
inherits the historical migration declarations and selection; descriptor metadata
is refreshed from the candidate. An explicit new migration synthesizes and records
a new historical context. Retaining a program retains its context, including failure pins.
The source digest continues to identify only the original source bundle.
`mo check NEW --upgrade-from DIR` reads the active bundle and its validated
context from a stopped data folder without modifying it. A source-file base
remains supported when it compiles without retained declarations. A tool-only
candidate inherits the historical context; an explicit new migration generates
its input declarations from the current old program.
These columns are part of the new version-2 format; no released version-2
folders predate them. Version-1 adoption is unchanged.

## Boundary, replies and catalogs

Before swapping, encode pending replies so no connection holds old heap cells.
Prepare a new loop, migrate and validate the state, remap queued handlers by
name, decode queued arguments against their target types, and reevaluate timer
intervals while preserving phase by name. A removed tool receives a structured
`tool removed by upgrade` reply only after commit, with no effect; removed timer
messages are dropped with an event. Prepared refusals leave the old queue intact.
After commit, remapped messages precede new arrivals and each commits before its
reply. Refresh every connection's descriptor, waiting-handler index and catalog;
recreate the immortal catalog process. Keep current and previous images alive
until no event, pending reply or connection borrows the previous image.

Phase 5 adds streaming transport, as Robert selected on 2026-11-08. This is a
prerequisite for live swapping: the current POST/JSON endpoint cannot deliver
unsolicited notifications. Add an authenticated MCP SSE stream using the same
service credential and connection limits. A successful activation queues
`notifications/tools/list_changed` after commit on each connected stream;
refusals send none. Here “each connected stream” means each connected client/session,
with one selected stream per session. Multiple streams in the same session must
not receive duplicate notifications; distinct sessions each receive one. This
pins the routing rule to the [MCP 2025-11-25 transport specification](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports).
Refresh the catalog before publishing the notification.
A reconnecting client refreshes `tools/list`; notification delivery is not a
durable event log. Bound pending stream output and close slow consumers rather
than delaying activation. Preserve existing POST/JSON calls. Advertise
`listChanged: true` only with this transport implemented and tested.

The transport work must test authentication, reconnect, slow readers,
disconnection during activation, notification ordering after commit, and a
fresh catalog on an existing session. Pin the MCP protocol version and stream
session semantics against its specification before implementation; the present
design does not claim the existing endpoint already supports these semantics.

## Budgets, refusals and evidence

The proposed live boundary-to-commit target is **1 second**. The sizing prototype
measured 0.239–0.270 seconds over five debug runs, including conservative extra
synchronization and retained data writes: a 3.7x margin over its slowest run.
This is an engineering target, not a guaranteed latency bound. It must be remeasured
on the task queue's 5,000 jobs with 2,048-byte payloads and 500 leases, including
encode, old-heap decode, identity migration, new validation, queue preparation,
retained snapshot encoding and durable commit. Report VM work, charged heap
peak, encoded bytes and each stage's elapsed time. Repeat on both debug and
optimized builds, record device/host and dispersion, and choose a stated margin.
Disk synchronization and host scheduling are not deterministic wall-time bounds.
Do not describe a work limit as a proof of a deadline.

Set live migration limits to 1,048,576 work units and 32 MiB heap; checked
restart to 16,777,216 work units and 48 MiB heap, with the existing 8 MiB stack
limit. Both heap sets fit within the 64 MiB service message-memory envelope.
The measured candidate heap peak, including queued arguments, is 12,565,784
bytes; the live allocation allowance has more than 2.6x margin. Identity uses
one VM instruction; that does not bound native encoding work. Implementation
charges native saved-codec work separately against the same remaining counter:
one unit per visited schema/value node, one per override name, and one per 64
input/output bytes rounded up. VM instructions reduce that same counter before
output validation and timer evaluation. These deterministic units bound traversals,
not allocator internals, collision handling or wall-clock time. Refusal preserves
output buffers, decoded output cells and the durable store. Revalidate these limits.
Neither set silently lifts ordinary handler limits. Also measure many generic
instantiations and maximum accepted nesting, not only an early nesting refusal.

Reserve tools codes E1414–E1431, respectively: credential, busy, stale base,
bundle size, bundle format, compile diagnostic, compile budget, missing
migration, migration signature, migration trap, migration budget, invalid
migrated value, changed access, timer interval, boundary deadline, recorded
source compile failure, source/start mismatch and ambiguous retained program.
Text and JSON use one refusal structure and include class, relevant identity
and bounded details, never credentials. Each code needs an errors fixture.
Refusals are events; successful activations are durable upgrade records.

Before approval:

- The task-queue old-declaration and saved-schema synthesis, schema equality/
  added-field controls, first-input/purity controls and sizing measurements are
  recorded in the preflight. General recursive synthesis and the production
  decoder override table remain implementation obligations.
- Reproduce the sizing results and assess the stated 1-second target and limits;
  the prototype is not the production activation transaction.
- Independently reproduce the preflight observations and retain failed predictions.

Before implementation is declared complete:

- Lean models name overrides explicitly, preserves `decode_refuses_other_schema`,
  and proves accepted snapshots validate and refusals preserve the modeled store.
- TLA+ explores upload, stage, boundary, commit, kill and restart; negative controls
  must violate single authority, message conservation and no cross-upgrade record.
- CBMC bounds bundle/page readers with reachable acceptance and refusal paths.
- Fuzz readers, migration input/output and compiler front ends; fault-inject every
  activation statement and commit outcome. State the finite bounds and assumptions.
- Run 1,000 seeded process-kill iterations across two upgrades with concurrent
  callers and old/new/invalid variants; reconcile acknowledged messages and every
  activation ordinal. This does not establish power-loss durability.
- Declare agent-evaluation scenario, clients, session count and pass gate before
  collection; keep development runs out of the denominator. Release requires
  Robert's separate approval.


## Live implementation

M4 builds the live path with these decisions; each is pinned by a test named in
`learnability/phase5/preflight/live/README.md`.

- Sessions are optional on POST: a request without `MCP-Session-Id` is served
  as before, an unknown one gets 404. A GET stream requires a known session (400
  without, 404 unknown) and `Accept: text/event-stream` (406). A second GET in a
  session replaces its stream, which is how one message never reaches two of a
  session's streams. At most 4 streams exist and each holds at most 1 KiB of
  unwritten output; a consumer that falls further behind, or leaves output
  unwritten for 10 seconds, is disconnected. Resumability (`Last-Event-ID`) is
  not offered. 64 sessions are kept; the least recently used is forgotten.
- Page refusals (wrong index, offset or total, non-canonical base64, wrong size,
  unknown or closed slot, digest mismatch) are E1418 and keep the slot, except a
  digest mismatch, which closes it. A slot also closes 60 seconds after its last
  accepted page unless a compile or commit is in progress.
- The commit waits for the compile and then for the next poll after the service
  step, when every pending reply has been encoded. The prepared loop, its catalog
  and the migrated state are built before the transaction, so any refusal up to
  the commit leaves the old loop untouched.
- The server keeps the current and the previous program. The previous one stays
  until the next upgrade so pre-upgrade events keep their handler names; the next
  activation waits until no request that began under it remains, and refuses with
  E1428 at the commit deadline. Events from older epochs render `<program not
  retained>`.
- An uncertain commit stops the loop with the store status; `mo serve` then
  exits 70 and a restart resolves the folder to one program.
- Refusals are events with only their code and the two source digests;
  successful activations are upgrade records and accepted events.
- `mo replay --failure ID --data DIR` without a source file chooses among the
  active and previous programs by image key, build and schema; distinct sources
  among the matches are E1431.

## Proposed host interface and stored representation

These declarations specify the boundary to implement; they are not current API.
The store treats bundles and digests as opaque validated bytes. All pointers
borrow caller-owned storage for the duration of the call.

```c
struct mo_store_program {
    uint8_t source[32], build[32], schema[32];
    uint64_t image, ordinal;
    const uint8_t *bundle;
    size_t bundle_size;
};
struct mo_store_activation {
    struct mo_store_program previous, candidate;
    struct mo_store_record previous_state, candidate_state;
    const char *migration, *report_json;
    size_t migration_size, report_size;
};
enum mo_store_activation_status {
    MO_STORE_ACTIVATED,
    MO_STORE_ACTIVATION_REFUSED,
    MO_STORE_ACTIVATION_UNSAFE
};
MO_MUST_USE enum mo_store_activation_status
mo_store_activate(struct mo_store *store,
                  const struct mo_store_activation *activation);
```

Both records have the current head sequence and their respective image keys.
The candidate ordinal is the previous ordinal plus one; overflow refuses before
writing. `REFUSED` requires successful rollback and confirmation of the old
head and program identities; uncertainty returns `UNSAFE`, requiring shutdown.
The operation compares the previous program inside its transaction. Opening
and first-program adoption use the same identity validation and version rules.

The strict-table columns are fixed as follows. Digests are 32-byte BLOBs and
image keys are eight-byte little-endian BLOBs, avoiding SQLite signed-integer
conversion. Ordinals are positive signed 64-bit integers; initial adoption is
ordinal zero. Snapshot records retain their existing framed binary encoding.

| Table | Columns and key |
| --- | --- |
| `program` | `id INTEGER PRIMARY KEY CHECK(id=1)`, `ordinal INTEGER`, `source BLOB`, `image BLOB`, `build BLOB`, `schema BLOB`, `bundle BLOB`, `context_digest BLOB`, `context BLOB` |
| `upgrades` | `ordinal INTEGER PRIMARY KEY`, `head INTEGER`, `old_source BLOB`, `new_source BLOB`, `old_image BLOB`, `new_image BLOB`, `old_schema BLOB`, `new_schema BLOB`, `build BLOB`, `migration TEXT`, `report TEXT` |
| `previous_programs` | `ordinal INTEGER PRIMARY KEY`, `source BLOB`, `image BLOB`, `build BLOB`, `schema BLOB`, `bundle BLOB`, `context_digest BLOB`, `context BLOB` |
| `previous_snapshots` | `ordinal INTEGER PRIMARY KEY`, `record BLOB` |

Every non-key column is `NOT NULL`; empty migration text means no migration.
Readers validate lengths, bundle digest, record framing and cross-table
identities before compiling or decoding. The retained rows' ordinal identifies
the activation that displaced them, while the active program's ordinal
identifies the activation that installed it. The upgrade table is append-only;
retention deletes only previous program/snapshot rows.

Reports use versioned JSON with `version: 1`, `status` (`accepted`, `refused`,
`already_active`), `code` (null on success), `class`, `message`, `base_source`,
`candidate_source`, `active_source`, and `migration`. Digests are lowercase hex;
unavailable identities are null. Accepted reports also carry `ordinal` and
`head` as decimal strings, avoiding JSON integer precision loss. The text
renderer uses the same class/message fields and prints every descriptor difference.
After both programs are compared, reports also contain `old_schema`, `new_schema`,
`implementation_changed` and the complete `differences` array. A refusal before
comparison omits those four fields; their absence means no comparison was made,
not that the programs match. Decimal-string VM work, native work, heap peak and
encoded-byte metrics are always present, including zero when that work did not run. Diagnostic detail alone is bounded to 4 KiB, truncated at
UTF-8 boundaries. Total JSON is bounded by two 1 MiB source bundles for original
handler identifiers, 80 framing bytes per old/new handler, and eight times the
detail bound for escaped text and fixed fields. Handler names are original source
identifiers, never expanded generic type names; required differences are not
silently truncated.

Every legacy folder with a snapshot is adopted through the activation transaction,
even if its image key still matches. Adoption retains that exact original snapshot
and records how many tail records were verified. Its unavailable old source is
encoded as 32 zero bytes in the upgrade row and rendered as null in history;
this sentinel is not a claimed source digest. A zero-record verification is
explicitly reported as providing no behavioral evidence.
A changed executable build also takes this verified adoption path when its
image key is unchanged. The new build digest becomes authoritative while the
old identity is retained, so failure records from both builds can pin their
programs correctly.

The event ring gains `upgrade` with outcome, epoch, source digests and refusal
code, without source bytes or credentials. Durable successful history comes
from `upgrades`; an evicted refusal event is not a successful activation.


## Offline rehearsal

`mo rehearse --artifact EXPORT --old-bundle OLD --candidate NEW
[--migrate Module.function] [--context OLD.mocc] [--json]` accepts a Phase 4 export and two canonical
source bundles. The export contains no source. Supply the separately recorded
MOCC context with `--context` when OLD retains historical generated declarations.
It is bounded and validated before compilation, and does not change MOSB source
identity. A tool-only candidate inherits it; a new migration generates its old
types from OLD compiled with that context. Require its build digest to
match the executing build and compile OLD to its recorded image key before
decoding. NEW is expected to have a different image key; do not apply the old
program-equality check to it. Run the same descriptor comparison, migration
limits and result validation as live admission. Exit zero means `would be
accepted`, exit one means refusal. The shared report also carries
`rehearsal: true`; it never means the live service accepted the candidate.
Successful rehearsal reports retain the exported `head` but omit `ordinal`:
no activation occurred and the export does not carry an activation ordinal.
Rehearsal opens no data folder and writes no service state. Live push repeats
all checks against its current base and state. Wrong build or wrong old bundle
refuses with both expected and actual identities.

## Acceptance contract

These are implementation gates, not claims about the current binary.

| Scenario | Required observation |
| --- | --- |
| Rename/add state field | Missing migration refuses; correct migration validates before activation; invalid result preserves old state |
| Add/remove tool | Report lists both changes; existing streams receive a post-commit list-change notification; refreshed catalog is current |
| Ten queued messages | In-flight message finishes first; new version handles queued calls in order, each persisted before reply |
| Live migration budget exceeded | Refuse without changing the service; checked restart can use the larger limits |
| Kill before/after activation commit | Restart resolves respectively to old/new program and state; migration never reruns after commit |
| Kill after commit before reply | Same digest reports already active; old base refuses with active digest |
| Local/proxied push | Identical admission and refusal text |
| Missing/wrong credentials | No compiler input or upload slot; credentials absent from diagnostics, events, records and proxy logs |
| Legacy folder and explicit upgrade | Exact-key adoption works; changed source requires explicit checked restart; lock acquired before reading program |
| Offline rehearsal | Same migration refusal/validation, mismatched build/old image refuses, no folder mutation |
| Restart after tool-only upgrade | Recorded bundle runs without source argument; stale source argument cannot roll back |
| Removed queued tool/timer | Tool receives post-commit removal reply with no effect; timer is dropped with event |
| Retention and old failure | Pinned original program remains replayable beyond ordinary retention count; ambiguity is reported |
| Access clause changed | Refuse with old and new clause names |
| Store failure | Confirmed rollback continues old service; uncertain commit stops and restart resolves one consistent state |
| New compiler build | Recorded source recompiles; changed image requires verified replay before serving |
| Non-loopback client destination | Refuse before reading either credential |
| Kill drill and agent evaluation | Reconcile acknowledged messages; publish every miss and leave phase open below the declared gate |
