# Production pause driver development

Final collection remains blocked on the nightly repair merge. No final latency
claim is made. The driver always labels output DEVELOPMENT and refuses reuse of
an output directory. Defaults are five repetitions, 5,000 distinct 2,048-byte
payloads and exactly 500 leases. A generated initializer fills the real task queue
without 5,000 updating HTTP calls; pagination checks every payload and lease before
and after unchanged-schema identity transfer. The candidate adds only a comment;
no migration function or --migrate option is supplied. The production branch
decodes directly using the candidate state type, then encodes and validates it.
Source schema and context remain unchanged. Historical reconstruction attempts
below remain evidence of a different path. The initializer's 2099
lease deadline prevents incidental expiry during development measurement; this
is a sizing fixture, not evidence for all reachable timer phases.

Commands from repository root:

```
python3 -m unittest discover -s learnability/phase5 -p test_measure_pause.py
python3 learnability/phase5/measure_pause.py --binary debug=build/pause-gcc/debug/mo --output build/pause-development-next --repetitions 1 --jobs 10 --leases 1 --build-description 'existing instrumented GCC debug; parent owns provenance'
```

Later, after integration authorizes collection and builds an isolated optimized
instrumented binary, run with both `--binary debug=/absolute/debug/mo` and
`--binary optimized=/absolute/optimized/mo`, omit count/repetition overrides,
and provide the exact compiler version and build command in `--build-description`.
Use a fresh `--output` directory on the disclosed measurement filesystem. Defaults
produce five debug and five optimized attempts. Raw stderr, push reports (including
VM/native work, heap peak and encoded bytes), generated source, stores and failed
attempts remain. Results record source/build-file/binary hashes, host and findmnt
filesystem/device information. Physical backing device and hypervisor storage
policy still require operator disclosure; findmnt cannot establish them.

The target is one second with a stated 2x margin: the largest observed boundary
must be at most 0.5 seconds. Summaries report each stage separately using sample standard deviation; a single
observation has no dispersion estimate. The initial driver used population
standard deviation and summarized only the total; integration corrected both
before final collection. Cached
old state is explicitly cache_hit_no_encoding; encoding occurs before the pause.
Nested probe records include synchronous stderr overhead and cannot be summed.
Store synchronization uses production defaults, without measurement overrides.

Development verification: three parser tests pass, including every missing stage,
null duration and wrong outcome. Ten jobs/one lease passed production serve/push
and complete before/after payload comparison in build/pause-development-small5;
the single boundary was 0.198383514 seconds. This small sample establishes driver
execution only. At that point, full-size initializer budget and five-per-build runs remained
unverified; subsequent full-size evidence is recorded below. Earlier directories retain syntax failure (small), missing data folder
(small2), and response_limit with ten-item pages (small4). Small3 passed before
payload comparison was added. Four-item pages corrected the response limit.
No checks or servers remain running.

Separate hostile compiler evidence for many generic instantiations and maximum
accepted nesting is missing here. Original preflight observations do not establish
production compiler stall bounds. No crash-safety, remote-client, evaluation or
release gate is closed by these measurements. Parent owns authoritative checks.

Integration full-size development attempt `build/pause-development-full1` used
5,000 distinct 2,048-byte payloads and 500 leases. Initialization and full pre-push
readback completed. Push refused E1424: 854,245 VM work + 194,331 native work
exhausted 1,048,576 units; heap peak 12,804,376 bytes, output bytes zero. The
refused boundary interval was 103,285,141 ns, not a durable-commit measurement.
The generated migration reconstructs every job and map; calling that identity
work was a failed assumption. The production unchanged-schema identity path must
be measured separately, without silently increasing its budget or discarding this
failed reconstruction attempt. All raw records remain in that directory.

Declared before execution: exactly one full-size DEVELOPMENT identity attempt
at `build/pause-development-identity-full1`, 5,000 distinct 2,048-byte payloads,
500 leases, existing unchanged instrumented GCC debug binary. No concurrent
collection, budget increase or production C change. This attempt is outside
FINAL collection; main remains d31c9301 and FINAL awaits merged verified repair.

During pre-push pagination, corrected the candidate comment delimiter from `//`
to Mo `#` in both driver and generated candidate, before compilation/push.
No activation attempt was made with the invalid comment.

The declared identity attempt committed ordinal 1 with migration null, identical
old/new schema digests and distinct source digests. The production
`identity_decode` record is `ok_no_migration`; no old_decode or migration_call
record exists. Budget usage: VM 5, native 619,287 (total 619,292), heap peak
11,967,960 bytes, encoded output 10,464,325 bytes. Boundary-to-commit was
540,073,616 ns (0.540073616 s): below one second, but **fails the required 2x
margin** (maximum 0.5 s). No retuning or second activation was attempted.

The command session terminated with status 143 during post-push pagination,
before the driver wrote its final results or verified-jobs file. The full
pre-push readback had completed. This is an interrupted driver attempt, not a
completed live before/after comparison. Raw reports, candidate check, sources
and store remain in `build/pause-development-identity-full1`; the outer log is
`build/pause-development-identity-full1.log`. A separately logged restart of
that committed store performs recovery readback without another push.

Source verification: `src/tools/upgrade.c` rejects schema differences without
a migration, inherits the active context in live compilation, and selects
direct candidate-type decode when migration is null. The design's tool-only
context rule agrees. The actual candidate check and accepted push both report
identical schemas. Five focused parser tests pass, preserving missing/null/
refused controls and adding duplicate and explicit-migration rejection.
Per-stage sample standard deviation remains null for one observation.

Recovery readback completed: all 5,000 ids and distinct 2,048-byte payloads,
500 leased states with exact 2099 deadlines and attempt count 1, and 4,500
ready states with attempt count 0 match the initializer. `next_id` was not
independently queried. `debug-0/recovered-jobs.json`, `recovery.log`, and
`recovered-results.json` retain this evidence and the reconstructed per-stage
summary (sample stddev null). This verifies the full persisted workload after
restart, while preserving the interrupted live comparison as a limitation.
No measurement/recovery processes or checks remain running. The original
reconstruction failure remains untouched. FINAL remains blocked.

## Exact snapshot validation before final collection

The driver now exports pre- and post-push state with `mo fetch` and runs a pure
`mo query` verifier against each snapshot. The verifier is an identical query-only
function in both fixture programs, never an exposed service tool. It checks map
size, every expected key and job id, each distinct 2,048-byte payload, every
attempt count and exact Ready/Leased state (including the 2099 lease deadline),
and next_id. Thus no duplicate, missing or extra key can pass. Its answer must be
one complete returned true value; errors and partial results fail validation.
Snapshot files, fetch/query stdout/stderr and snapshot hashes are retained.
This replaces the expensive four-job-page readback outside the measured interval;
it does not change the production activation or its measured boundaries. The
query has the normal offline query limits, not a lifted live migration limit.

Development proof: `MO_MEASUREMENT_BINARY=/tmp/mo-eval-build-aanyuygb/build/gcc/debug/mo
python3 -m unittest discover -s learnability/phase5 -p test_measure_pause.py -v`
passed six tests in 16.312 seconds. The real-state control compares ten jobs/one
lease with the old paginated readback, then separately rejects altered payloads,
next_id, attempt counts and lease deadlines. No full-size speed claim follows
from this small control; full-size validation remains part of frozen collection.

One ten-job/one-lease instrumented DEVELOPMENT smoke then passed end-to-end:
`python3 learnability/phase5/measure_pause.py --binary debug=build/pause-gcc/debug/mo
--output build/pause-validator-small1 --jobs 10 --leases 1 --repetitions 1
--build-description 'DEVELOPMENT existing instrumented binary; exact snapshot validator smoke only'`.
This reused the existing pre-freeze instrumented binary without rebuilding it;
its provenance and timing remain development-only. Both exact snapshots and the
successful production push/timing records are retained in that output directory.
The prior refusal and interrupted-readback artifacts remain unchanged. No
measurement or service processes remain running from these controls.

## Full-size export-validator timeout

`build/pause-development-snapshot-full/results.json` records a pre-activation
`mo fetch` timeout at 300 seconds with 5,000 jobs. No pause sample was collected.
The original subprocess helper did not persist partial timeout stdout/stderr;
the failure string, source, store, service logs and temporary export remain. This
retention defect is corrected. Observation commands now have an 1,800-second
limit, independent of the unchanged activation budgets and 0.5-second margin.
A separately named development retry retains its own results.

The 10-job/one-lease timing-enabled ASan/UBSan control passed at
`build/pause-sanitize-control/results.json`; it is not production sizing evidence.

## Full-size live verifier correction

The longer export retry was deliberately interrupted after 380.641 seconds in
the pre-activation fetch (`build/pause-development-snapshot-full-retry`, exit
130). Its command/elapsed receipt and raw service files remain; partial client
output was unavailable on operator interruption. No activation sample occurred.

A fixture-only read-only `inspect_fixture` tool invokes the same exact pure
verifier used by the offline snapshot control. The first standalone attempt
failed because the development script had not created the data directory; that
E1409 log is retained. The corrected full-size call returned true in 0.4661
seconds. Both old and candidate bundles expose this verifier unchanged. It
checks every id, payload byte, attempt, state, deadline, map count and next_id.
These calls run before and after activation, outside the measured interval.
Eight tests passed in 17.383 seconds, cross-checking live verification against
pagination and snapshot query and rejecting each corrupted fixture.

The full-size development production push with this verifier passed all state
checks (`build/pause-development-live-full/results.json`). Boundary-to-commit
was 0.502645410 seconds: again below the 1-second target but above the declared
0.5-second margin. It is one development sample, collected while checks ran,
not the final five-repetition result. No gate or work budget changed.
