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
