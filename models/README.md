# Model checks

Run `bash ci/check.sh models`. Java and the TLA+ tools v1.8.0 jar are required.
Download `https://github.com/tlaplus/tlaplus/releases/download/v1.8.0/tla2tools.jar`
to `build/models/tla2tools.jar`, or set `TLA2TOOLS_JAR`. The runner verifies SHA-256
`e20447644bfabd29d6bd0fc2c0eb1473fb301c1a0e4dfe1961938ce5a191edbf` before execution.
Missing tools are a local skip and fail under `CI=1`; a skip is not evidence.
Logs and generated counterexample traces remain under `build/models/`.

## Export generation checks

The finite instance has one kind, three generation IDs, three sequence/state values,
two-byte artifacts and at most two restarts. IDs represent collision-free random
identities; TLC does not prove operating-system entropy quality. A fetch starts with
its first pin and requests successive pages. Replace and restart preserve that fetch's
stale identity. Expiry/release are abstract actions, not a proof of clock sampling.
`NoMixedGeneration` checks both identity and byte provenance for every accepted page.
`NoReleasedRead` checks a history bit so a later release cannot hide a forbidden read.

The progress instance disables replacement, expiry and restart. It assumes weak fairness
of creation, reading and acceptance; `Completes` requires eventual acceptance. Commits
remain enabled. The general safety instance makes no unconditional progress claim.
Two negative configurations must produce invariant violations: sequence-based identity
must violate `NoMixedGeneration`, and bypassed release must violate `NoReleasedRead`.
These checks do not prove C, transport delivery or output publication.

| Model action | Implementation and regression |
| --- | --- |
| CreatePin / Replace | `mo_protocol_export`, independently copied `mo_export_create`; `export_generations` creates twice at one sequence |
| Commit | existing service commit path; C trace reads old `a` bytes after `change`, Python verifies the entire old artifact |
| ReadPage | ID comparison before `mo_export_page_bounds` and copying, fixed offsets and count; CBMC checks exact arithmetic acceptance |
| Expire | `mo_export_expire`, poll deadline and page-time check; C fake clock expires the pin |
| Restart | supervisor restart and server free release the arena, new server starts empty; C stale-ID refusals for both |
| FetchAccept / final release | C final page release and Python SHA-256/coverage verification; U9 owns the actual fetcher and publication |

Development failure retained: the first release-negative-control run exposed an
unparenthesized TLA disjunction in the history-bit assignment, leaving a successor
variable unspecified. The runner rejected that error instead of treating any nonzero
exit as a detected invariant. Parenthesizing the expression yields the required
`NoReleasedRead` counterexample. The initial trace is retained in the local build logs.

## Failure capture checks

`Capture.tla` models one service's failure capture beside its application work. The finite
instance has three kinds, at most four failures, a three-record retention cap and at most two
successful updates. TLC explores about 108,000 distinct states in a few seconds.

- `Apply` and `Snapshot` are the only actions that change application state or the log
  position, and they run only between captures. `Fail` records both values, so
  `RecoveryUntouched` fails if any capture action changes them.
- `Retain`, `Evict` and `RetainRefuse` apply `src/host/store.c` retention to the record
  count. A repeat deletes only its own kind's previous latest. If that leaves no room, the
  repeat is refused and no other kind loses a record. A new kind that finds the store full
  evicts the kind whose latest record is oldest, first and latest together.
  `Retention` checks the cap and the one-first, at-most-two shape of each kind;
  `RepeatKeepsOthers` checks that a repeat never deletes another kind's record.
- `Commit` writes both halves and the evictions at once and publishes the id.
  `CommitUncertain` is a `COMMIT` that reports an error: the transaction may or may not be
  durable, no id is published, and the service stops. `Reopen` restarts a stopped service, so a
  later view can list a pair whose id was never published. `RollbackFail` also stops the
  service. `Rollback` is a clean refusal. `Crash` may happen while writing, committing or
  before counting. `PairComplete` checks that every published or retained id has both halves.
- `CountedOnce` checks one supervisor count per failure whatever the capture outcome, and
  `NoUnsafeContinue` checks that an uncertain store never returns to idle without a restart.

The byte cap (64 MiB) is not modeled; only the record-count cap is. SQLite atomic commit is a
modeling assumption, checked separately at process-kill boundaries in `test_service_store`; this
is not a power-loss proof.

Each invariant has a negative control, selected by the `Bug` constant, that must produce its
named counterexample:

| Configuration | `Bug` | Mutation | Invariant violated |
| --- | --- | --- | --- |
| `CaptureBadPublish.cfg` | `publish` | `Write` publishes the id before commit | `PairComplete` |
| `CaptureBadCount.cfg` | `count` | `Rollback` counts the failure a second time | `CountedOnce` |
| `CaptureBadRecovery.cfg` | `recovery` | `Commit` also advances the recovery log | `RecoveryUntouched` |
| `CaptureBadUnsafe.cfg` | `unsafe` | `Count` returns to idle after an uncertain outcome | `NoUnsafeContinue` |
| `CaptureBadRetention.cfg` | `bound` | A new kind is inserted without eviction | `Retention` |
| `CaptureBadRepeat.cfg` | `repeat` | A repeat with no room evicts another kind | `RepeatKeepsOthers` |

Development failure retained: the first run of the strengthened model found a crash after
`CommitUncertain` reaching `recover` with an unsafe handle, which the first `NoUnsafeContinue`
rejected. A crashed process is no longer running, so `recover` joins `count` and `stopped` as
an allowed phase; `Recover` keeps the handle unsafe, so the service still stops. Temporary
reachability invariants confirmed that `RetainRefuse`, `Evict` and an unpublished pair after
`CommitUncertain` are each reached in the passing configuration.

## Live upgrade swap checks

`Swap.tla` models one service process swapping its whole program while clients call it. The
finite instance has three programs, three client messages, each submitted once, and at most
two activations. Message 2 calls a tool that program 2 removes and program 3 restores. Crashes,
restarts and snapshots are unbounded; none of them grows the state. TLC explores about 29,000
distinct states, depth 20, in about a second.

The durable store is the program row (`prog`, `ord`), the append-only upgrade table, the
snapshot with its key and the log tail, where each record carries the key of the program that
appended it. `History` is what a restart replays: the snapshot followed by the tail. Everything
else (mailbox, upload slot, staged candidate, unsent replies) is volatile and lost on a crash.
The live boundary, re-admission and removed-tool replies are the contract in
`docs/design/upgrades.md` (Boundary, replies and catalogs). When this model was written, live
swapping was not implemented in C, so those rows name the design, not a regression.

| Model action | Implementation and regression |
| --- | --- |
| Submit | a tool call enters the service's FIFO mailbox |
| Handle | the live program's handler, then `mo_store_call` `APPEND` of one record under its key; the reply waits for that commit |
| HandleRemoved | `tool removed by upgrade` reply after commit, no record, no effect |
| Reply | the acknowledgement for a committed record |
| Snapshot | `mo_store_call` `SNAPSHOT`: the snapshot replaces the old one and clears the tail |
| Open / Stage / StageRefuse | an upload slot opens only on the active base; pages arrive; the last page stages a compiled candidate or refuses |
| CommitRequest | commit names the base again and is checked against the active program |
| Committed / Refused / Uncertain | `mo_store_activate` in `src/host/store.c`, `MO_STORE_ACTIVATED`, `MO_STORE_ACTIVATION_REFUSED` with confirmed rollback, `MO_STORE_ACTIVATION_UNSAFE`; enabled only at a message boundary (no reply pending) |
| Crash / Restart | restart reads the program row, restores the snapshot and replays the tail with that program; `serve_upgrade` in `src/tools/upgrade.c` reports `candidate source is already active` and never reruns a migration |

A push of the active candidate, or of a stale base, is answered without changing anything, so
those pushes are stuttering steps; the base guard on `Open` and `CommitRequest` is what stops
a re-push after a crash from rerunning a migration. Committed leaves the mailbox unchanged:
queued messages are re-admitted in their original order ahead of later arrivals, and a queued
call to a removed tool reaches `HandleRemoved` under the new program.

- `SingleAuthority`: a serving process runs the program in the durable row, an uncertain commit
  leaves nothing serving until restart, and the row's ordinal names the last upgrade record.
  Because `Handle` appends under the live program, this also means every message is handled by
  the program that is durable at that moment.
- `MessageConservation`: no message appears twice in `History`, every acknowledged message
  appears exactly once, and a message answered `tool removed by upgrade` never appears.
- `NoCrossUpgradeRecord`: the snapshot and every tail record carry the durable program's key,
  so replay never applies a record under a program other than the one that appended it.

Each invariant has a negative control, selected by the `Bug` constant, that must produce its
named counterexample:

| Configuration | `Bug` | Mutation | Invariant violated |
| --- | --- | --- | --- |
| `SwapBadAuthority.cfg` | `authority` | the old program keeps serving after an uncertain commit | `SingleAuthority` |
| `SwapBadConservation.cfg` | `conservation` | removed-tool replies are sent before the outcome is known; a refusal leaves the queue intact and the old program applies the answered call | `MessageConservation` |
| `SwapBadCrossRecord.cfg` | `cross` | the activation keeps the old snapshot and tail, only rewriting the key | `NoCrossUpgradeRecord` |

These checks do not prove the C code, SQLite durability or atomic commit (an assumption, as in
the capture model), behaviour on power loss, or any wall-clock budget for the boundary or the
transaction. Client retries are not modeled: a message lost with the mailbox is never
resubmitted, so at-least-once delivery from clients is outside this model. Migration contents are
abstract: the model checks which messages the migrated state covers, not its values.

Development failure retained: the first cross-record control kept the tail but still wrote the
snapshot at the head, so replay applied the tail twice, and TLC reported `MessageConservation`
before `NoCrossUpgradeRecord`. The control now keeps the old snapshot and tail together, which
breaks only the key. Temporary reachability invariants confirmed that the passing configuration
reaches a second activation, a removed-tool reply under program 2, a call to the restored tool
appended under program 3, an uncertain commit both durable and not, a re-admitted queue after
commit, and a crash with an applied but unacknowledged message.
