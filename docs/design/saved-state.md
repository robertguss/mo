# Saved service state, version 1

MO-158 implements D262, D263 and the MO-151 validation boundary. This is an
internal disk format, not a Mo API. Phase 3 requires the same bytecode image
for recovery. Phase 5 must explicitly migrate between schemas and images.

## Ownership and transactions

`heap/saved.c` owns bounded encoding, decoding and validation against trusted
runtime type descriptors. `host/store.c` owns SQLite connections, transactions
and record envelopes. `vm/service.c` owns handler selection and replay. Neither
host nor heap calls a Mo handler. Core, Lean and the bytecode instruction set
are unchanged.

The embedding owns one store per service and keeps it alive across service
restarts. The runtime alone receives the store capability. Its `LOAD`, `APPEND`
and `SNAPSHOT` operations are declared as `STOREOP` entries in `hostops.def`.
They have no Mo signature or bytecode opcode. Ordinary `HOSTOP` readers ignore
them. A NULL path selects an in-memory SQLite database for tests, using the
same transactions and record format. Other paths must name disk files. Empty
paths, `:memory:`, and existing zero-byte files are refused.

SQLite 3.54.1 is vendored unchanged, with its archive digest and public-domain
dedication recorded in `vendor/sqlite/README.md`. `SQLITE_ZERO_MALLOC` removes
its default allocator. Before initialization, host installs the bounded
callbacks in `base/store-memory.c`, which `base` owns because only `base` and
`heap` call `malloc`. Each block is charged its exact size plus a small header,
under a mutex, before it is allocated. All stores in a process share one
budget, `MO_LIMIT_STORE_MEMORY`, and at most `MO_LIMIT_STORES` (8) stores are
open at once; another open fails with `MO_STORE_LIMIT`. Each connection's page
cache is `MO_LIMIT_STORE_CACHE_KIB` (256 KiB). A disk store's largest operation
measured about 17 MB of SQLite memory with a 16 MiB record, and an idle one
about 0.3 MB, so the budget allows 24 MiB per store and every disk store can
run its largest operation at the same time, provided no in-memory fake holds a
large state. Reallocation temporarily charges both
blocks and preserves the original on failure. This budget is separate from the
service process heap. The in-memory fake keeps its whole database in the same
budget, so it suits tests and simulations with small states; a fake write that
does not fit fails like a disk write.

Disk stores use rollback journals (`journal_mode=DELETE`) and
`synchronous=EXTRA`, including SQLite's directory synchronization after journal
deletion. The parent directory must already exist. Durability depends on the
filesystem and device honoring synchronization. No power-loss test or remote
backup guarantee is implied by the process-crash tests.

The database contains three strict tables:

```sql
CREATE TABLE head(id INTEGER PRIMARY KEY CHECK(id=1), seq INTEGER NOT NULL) STRICT;
CREATE TABLE snapshot(id INTEGER PRIMARY KEY CHECK(id=1), record BLOB NOT NULL) STRICT;
CREATE TABLE log(seq INTEGER PRIMARY KEY, record BLOB NOT NULL) STRICT;
```

The head sequence has its own row so that an append rewrites a few bytes, not
the snapshot. `head` lets recovery detect lost log records at the end, which
consecutive sequence numbers alone cannot.

Host verifies the table definitions, disables triggers and views, disables
trusted schema features, and enables SQLite defensive mode. There is no SQL
interface for handlers. SQLite SQL is bounded by `MO_LIMIT_STORE_SQL` (4096
bytes). Every record payload, whether a snapshot's encoded state or a log
message, is at most `MO_LIMIT_SAVED_BYTES` (16 MiB). Its frame adds 34 bytes,
and SQLite's row limit (`MO_LIMIT_STORE_ROW`) leaves room for that frame and
SQLite's own record header, so the store accepts every payload the runtime can
produce. The runtime refuses a state-changing message with `MO_SERVICE_MEMORY`
before queueing it when its log payload would exceed the limit.

The initial snapshot is sequence zero. Its transaction sets `user_version=1`.
An initialized store missing its head or snapshot row is corrupt, not empty.
`APPEND` inserts exactly the next sequence and advances `head` in one
transaction.
`SNAPSHOT` replaces the snapshot at the current head and deletes the covered
log in one transaction. `MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES` is 100. Recovery
allows a 100-message tail because a crash or write failure can occur between
that append and the corresponding snapshot commit. An open that recovers to a
due sequence whose snapshot is missing takes it then; an open whose snapshot
already covers the head writes nothing.

For each successful state-changing handler, the runtime encodes the message,
commits it, and takes a snapshot when due before publishing state or replying.
Read-only replies, application `Error` replies and failed handlers write
nothing. A write failure stops the service with `MO_SERVICE_STORE`; it does
not increment D308's handler-failure counter. The commit outcome can be
uncertain. Reopening recovers any committed message, so callers cannot assume
that retrying the same request is idempotent. Request deduplication is not
part of this format.

`LOAD` reads a consistent snapshot and consecutive log records. Recovery stages
state privately and publishes nothing until the entire transaction succeeds.
It validates each message's handler, update classification, arrival time,
arity and typed arguments before calling the handler. Replay has the same
work and heap limits as live calls, checks the resulting-state checksum and
emits no replies. Timers restart with one fresh tick each; elapsed downtime
does not generate historical ticks. Committed ticks in the tail are replayed
with their original arrival times. A replayed handler that exceeds its heap
or work budget reports `MO_SERVICE_MEMORY` or `MO_SERVICE_WORK`, not invalid
data. A heap exhaustion inside the value decoder is not distinguishable from a
malformed value and reports invalid data.

SQLite creates the database file empty and fills it in the first commit. A
crash during the very first open therefore leaves a zero-byte file, which a
later open refuses like any other truncated store. No message was acknowledged
from such a file, so deleting it is safe; the runtime does not do so itself.

## Record envelope

All integers are little-endian, without padding. Signed values use two's
complement. No address, native struct layout or runtime type index is stored.
Every read checks the remaining span before advancing. Trailing bytes are
invalid. FNV-1a-64 checksums use offset 14695981039346656037 and multiplier
1099511628211, with arithmetic modulo 2^64. They detect accidental corruption,
not deliberate modification or rollback of an entire valid database.

| Field | Bytes | Meaning |
| --- | ---: | --- |
| Magic and version | 5 | `4d 4f 53 54 01` (`MOST`, version 1) |
| Kind | 1 | 1 for a log message, 2 for a snapshot |
| Sequence | 8 | Unsigned, at most INT64_MAX |
| Image key | 8 | Bytecode checksum XOR (service descriptor index shifted left 32) |
| Payload length | 4 | Exact number of payload bytes |
| Payload | variable | As specified below |
| Checksum | 8 | FNV-1a-64 of every preceding byte |

A snapshot payload is one encoded value of the service's state type.
A message payload has these fields, in order:

1. Timer flag, one byte, exactly 0 or 1.
2. Handler table index, four bytes, in the tool or timer table respectively.
3. Arrival time, eight signed bytes of UTC microseconds, within years 1 to 9999.
4. Argument count, four bytes. A timer has zero arguments.
5. FNV-1a-64 of the encoded resulting state, eight bytes.
6. Each argument in declaration order, with a four-byte byte length followed
   by its complete encoded value. State and arrival time are not arguments here.

## Encoded values

An encoded value begins with `4d 4f 53 56 01` (`MOSV`, version 1), a four-byte
schema length, the schema bytes, and the value payload. The decoder generates
the expected schema from the trusted runtime descriptor and compares it byte
for byte. It never builds runtime types from untrusted schema bytes.

Schema traversal assigns IDs from zero on first visit, before visiting children.
A repeated type is byte 255 followed by its four-byte traversal ID. A first
visit starts with one fixed type tag:

| Tags | Types in tag order |
| --- | --- |
| 0 to 6 | Unit, Bool, Byte, Int, Float, Char, Time |
| 7 to 12 | Duration, Decimal, Range, Text, Bytes, BigInt |
| 13 to 18 | Tuple, List, Set, Map, Record, Choice |

Record and Choice then carry their nominal name. A name is its four-byte UTF-8
byte length followed by its bytes; absent names have length zero. Every first
visit then carries a four-byte type-parameter count and those parameter
schemas in order. Next comes a four-byte group count: the case count for
Choice, one otherwise. Each Choice group starts with its case name and a
one-byte unnamed-fields flag. Every group has a four-byte field count and,
for each field, its name and type schema in declaration order. There are no
schema entries for functions, environments or capabilities, even in empty
collections. Names and ordering are retained exactly, with no normalization.

Value payloads have these layouts:

| Type | Payload |
| --- | --- |
| Unit | No bytes |
| Bool, Byte | One byte; Bool is exactly 0 or 1 |
| Char | Four-byte Unicode scalar, excluding surrogates |
| Int, Duration | Eight signed bytes |
| Float | Eight bytes of IEEE 754 binary64 bits |
| Time | Eight signed microsecond bytes, validated against the runtime time limits |
| Decimal | Sixteen signed bytes; the reserved minimum 128-bit value is invalid |
| Range | Start and end, eight signed bytes each |
| Text, Bytes | Four-byte byte length, then bytes; Text must be valid UTF-8 |
| BigInt | One-byte sign (0 zero, 1 positive, 2 negative), then length and little-endian magnitude bytes; zero has empty magnitude, nonzero magnitude has a nonzero last byte |
| List | Four-byte element count, then typed element payloads |
| Set | Four-byte element count, then distinct elements in insertion order |
| Map | Four-byte entry count, then distinct key/value pairs in insertion order |
| Tuple, Record | Four-byte field count, then fields in descriptor order |
| Choice | Four-byte case index, four-byte field count, then that case's fields |

Nested values omit schema and magic. Sharing is expanded, not serialized as
native links. Recursive schemas use backreferences; value trees do not.
Decoding rejects duplicate map keys and set elements rather than silently
discarding them. The selected layout comes from the trusted image, not disk.

Both schema and value traversal stop at 128 levels (`MO_LIMIT_SAVED_DEPTH`).
Values are bounded by 1,048,576 visited nodes (`MO_LIMIT_SAVED_NODES`), the
existing collection and byte limits, the 16 MiB payload limit and the remaining
heap budget. Failure leaves the output unchanged and releases partial values.

Because sharing is expanded, a decoded value can need more heap than the live
value it came from. A store-backed service therefore passes the initial state,
each state-changing tool's arguments and each new state through encode and
decode in its own heap before accepting them. The live service then holds the
same unshared values that recovery will build. A value that cannot be copied
this way is refused with `MO_SERVICE_MEMORY` before anything is committed, so
an acknowledged update cannot be too large to recover. A supervisor restart
keeps the in-memory state and does not reread the store, so restart never
needs two copies of the state.

## Refusal and verification

`MO_SERVICE_SAVED_INVALID` identifies invalid or incompatible saved input:
content checks, SQLite's own corruption codes, and a stored value longer than
the runtime ever writes (`SQLITE_TOOBIG`). Any other SQLite failure
while loading, such as an I/O error or an exhausted store budget, is
`MO_SERVICE_STORE`, because a retry may succeed and the data may be intact.
`mo_service_status_text` describes it as `saved service state or message is
invalid; recovery refused`. A store operation failure is `MO_SERVICE_STORE`,
described as `service store failed; reopen it before retrying`. These proposed
embedding descriptions require Robert's approval before merge. No CLI command,
reference wording, network endpoint or public simulation API changes here.

`tests/test_saved_state.c` covers the scalar and compound encoding contracts,
exact Int bytes, every truncated prefix, trailing bytes, depth bounds and
cleanup. `tests/test_service_store.c` covers disk and fake-store replay,
snapshot pruning, read-only and Error exclusion, saved arrival times, timer
replay, hostile database schemas and triggers, malformed typed payloads with
valid record checksums, process exits before append, after commit but before
reply, and during an uncommitted snapshot, a rolled-back append, a failed due
snapshot, payloads at the size limit, shared values that would expand past the
heap on recovery, and Map and Set recovery through a freshly loaded image. Fuzz targets cover raw and
checksum-repaired frames, full service recovery and typed value payloads.
