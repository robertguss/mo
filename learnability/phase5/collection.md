# Collection declaration and results

Final collection declaration (development runs are recorded separately): 1,000 process
kills, seed 20261110, with concurrent callers; the existing 15-kill drivers smoke
remains development evidence. Declare any development count before running it.

Extend `ci/check_kill_loop.py`, not a second implementation. It now covers tool-only changes, changed access, a field migration, broken
migration, oversized bundle and stale base. Pin each expected refusal to its code/class;
retain changed-access coverage. The model must preserve every acknowledged job
transition exactly once, allow only pre/post outcomes for lost replies, check
migrated revision separately and reconcile every activation ordinal after each
folder-only restart. Successful migration must preserve jobs and leases rather
than use the candidate initializer.

Declare kill classes: ordinary request, upload in progress, compiled/staged before
commit, migration/boundary in progress, activation transaction and after commit
before observed reply. Record observed coverage independently of requested timing;
a random delay is not evidence that a kill landed in a particular stage. Retain
existing store fault injection/model evidence for internal boundaries that cannot
be observed externally. Missing class coverage remains a reported gap, never a
reason to label a timing guess as coverage. Retain seed, count, every failure,
accepted/unknown/refused calls, pushes, activation identities and restart results.
Process kills do not establish power-loss durability.

## Production pause measurement declaration

Use 5,000 jobs, distinct 2,048-byte payloads, exactly 500 leases, and the actual
production activation path. Measure five debug and five optimized repetitions on
the same stated host/device/filesystem; retain all samples, including refusals.
Record source and binary hashes, compiler flags, kernel, CPU, memory, filesystem,
backing device and synchronization settings. Report min, median, max and sample
standard deviation per build and stage. State a target margin of at least 2x
against the 1-second engineering target (slowest observed pause <= 0.5 seconds);
a miss is published and does not silently change the target or the work budget.

Stages: old-state encoding, old-heap decoding, identity migration, new-state
validation, queued-argument preparation, retained-snapshot encoding and durable
commit. Measure boundary-to-commit separately from compile/upload and request
round-trip time. Record VM work, native charged work, charged heap peak and encoded
bytes. A frozen fixture-only read-only verifier checks every job field and
next_id before and after the push, outside the measured interval. Its negative
controls are compared with pagination and offline snapshot queries. The
production workload has no queued caller arguments; `swap_prepare` records
that actual case, not the prototype's separate 64-argument stress case. Native accounting does not cover allocator internals, hash collision work,
scheduling or disk latency. The earlier transfer prototype cannot replace this
measurement. Production stage instrumentation is implemented; development observations are
kept in measurement-development.md and do not close this collection requirement.

## Parent commands and remaining work

Development checks available now, without service collection:

```sh
python3 -m unittest discover -s learnability/phase5 -p test_evidence.py
python3 learnability/phase5/evidence.py build-inputs . > build/phase5-build-inputs.json
python3 learnability/phase5/evidence.py build-inputs . --compare build/phase5-build-inputs.json
```

After extending and verifying the drill, merging and verifying the nightly repair,
and freezing the candidate, the declared long drill command is:

```sh
MO_KILL_COUNT=1000 MO_KILL_SEED=20261110 bash ci/check.sh drivers
```

The extended driver covers the declared variants. Supplemental debugger controls
in `ci/check_kill_stages.py` observe six internal kill classes; retain their
source/binary identity and results separately from random-delay coverage. Final
collection still requires the completed verification and frozen inputs.
The parent also runs `bash ci/check.sh gcc` and `bash ci/check.sh drivers static`
after final documentation changes, waiting for completion in the same turn.
Evaluation uses `evaluation/harness.py run SPEC OUTPUT --slot N` after its
freeze check; measurement uses `measure_pause.py --freeze MANIFEST` with exactly
five repetitions for each debug and optimized binary. The Phase 4 diagnostic
`round` command is not a substitute.

## Completed collection

The declaration above was retained unchanged through collection. The 1,000-kill
run passed with seed 20261110; six separate frozen-binary debugger controls
passed, with the ordinary-request breakpoint observed in the parser VM. Five
debug and five optimized production samples completed; the debug series missed
the declared twofold margin. The original four-session experiment 2 scored 0/4
and leaves the phase open. See `results/` and `docs/status/phase5-run.md` for raw
records, corrected diagnostic analysis and release limits.
