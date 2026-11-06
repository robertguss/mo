# Failure capture development evidence

This is implementation evidence, not prospective evaluation. The pilot remains
30 attempts/29 completed grades, with pilot7 interrupted, four uncalibrated
variants and no independent baseline. No private pilot directory was read.

## Predictions and scope

The canonical design's capture invariants predict complete published pairs,
unchanged application recovery, one original-failure count and terminal unsafe
store outcomes. The implementation checks below test those predictions. The
storage sizing run is an empirical measurement, not a predeclared performance
claim. No code review or complete verification gate is claimed by this receipt;
the host's authoritative runs and independent review remain required.

## Focused results

- Initial foundation GCC run: `build/failure-capture/gcc-initial.txt` ends
  `check.sh finished: pass`. This predates the complete implementation.
- `make -j8 CC=gcc build/gcc/debug/tests/test_service_store.test.run`:
  `final-inventory.txt` passes. Coverage includes retained first/latest and
  distinct-kind overflow, clean rollback and later snapshot, committed-but-error
  and rollback failure, poisoned handles, read-only nonmutation, exact-limit
  updating arguments and equally large read-only arguments, work/heap/timer
  captures, native capture allocation refusal and maximum 16 MiB pre-state.
- Store child-process crash points after retention deletion, after state insertion
  and after commit expose either the old complete pair or the new complete pair.
  These are process-crash tests, not power-loss tests.
- Runtime fault tests count the original failure once even when diagnostic storage
  is unsafe, then stop; clean refusal leaves the service running.
- Focused protocol-server and CLI suites pass in `final-focused.txt`.
- `python3 learnability/phase4/failure-capture/smoke.py build/gcc/debug/mo
  build/failure-capture/smoke-final2` passes. The real loopback service records
  fifteen failures, retains ids 1 and 15 across 101 intervening updates and a
  snapshot, captures the final stopping failure, and exports/fetches a verified
  kind-2 artifact with mode 0600. The transcript, stderr and summary are retained.
- `TLA2TOOLS_JAR=/tmp/mo-u9-tla2tools.jar python3 ci/check_models.py` passes all
  Capture and existing Export instances and their negative controls. The jar
  matches the pinned SHA-256. The direct release download did not match; the
  failed pin check is retained and was not bypassed or repinned.
- `bash cbmc/prove.sh slice` and `bash cbmc/prove.sh --reach slice` pass. Scope:
  actual length-prefixed slice arithmetic over at most 32 bytes, arbitrary valid
  cursor and limit, unchanged output on refusal. UTF-8's callee result is
  unconstrained; this is not a whole-record or SQLite proof. The first reach
  harness omitted text branches and failed; its result is retained.
- `build/fuzz/bin/fuzz_service_failure -max_total_time=10 -timeout=5
  -artifact_prefix=build/failure-capture/ build/failure-capture/fuzz-corpus`
  completed 779,622 executions in 11 seconds without a crash. This bounded smoke
  predates the final arrival-time guard; the authoritative run must rebuild it.

## Storage sizing

`test_failure_sizing` writes a maximal state and near-maximal message three times
(first/latest/replacement). Payloads are opaque storage fixtures; the separate
runtime test establishes successful capture of a valid maximal pre-state.
The record pair is 17,825,348 bytes including both MOST frames. The run took
0.536139 seconds for three transactions, peaked at 56,644 KiB RSS from a 21,592 KiB
baseline, and left a 35,708,928-byte SQLite file for two retained pairs. These
figures include native input/frame buffers and SQLite overhead, not just one
state record. See `sizing.json`; timings are specific to this VM/build.

## Preserved failures and verification handoff (historical)

`failed-checks.txt` retains initial missing-table, missing-API, discarded-error-code,
compiler and fixture failures. Early smoke drivers omitted the data directory or
protocol-version header, then used the wrong argument representation; these
failed runs are retained and are not implementation passes. A final smoke attempt
caught invalid inline Mo syntax in the omission-count addition, corrected before
`smoke-final2` passed.

At this intermediate handoff, the parent still owned final GCC/Clang/ASan+UBSan suites, all-build drivers, static,
new-reader fuzz rerun, relevant existing export proofs/reach, the long recovery
kill loop, simplification and actual review. No cross-provider review, full
verification closure, publication or shipping is claimed here.

## Review remediation (local, before final authoritative rerun)

The parent completed the actual local review in `build/failure-capture/review`.
This follow-up resolves its six accepted remaining findings: failure continuation
selectors, stale guide availability, numeric omission counts, structured signatures
with the precise recorded limit value, bounded metadata catalog/indexed pair lookup,
and the SQLite refusal matrix. The export consumer retains its already-verified
kind/id instead of verifying the full artifact twice. A continuation that changes
the failure id for the same pin is refused.

`review-fixes-build4.txt` passes store and protocol-server tests. New checks cover
TOOBIG/NOMEM/CONSTRAINT/BUSY clean refusals followed by ordinary snapshots and later
captures, CORRUPT/IOERR and failed post-rollback head reads poisoning the handle,
one original runtime failure, live frame/span and top-frame variable context,
byte-cap eviction before the record-count cap, and refusal to grow an existing
kind by evicting another kind. Catalog reads and indexed selection succeed despite
an unrelated corrupt state blob; selecting that blob refuses. Catalog pages inspect
at most eight bounded messages, report the exact remaining count and never read
state payloads. Their inspected message bytes are charged to the request budget.

`review-lower.txt` passes, including numeric omission/continuation and structured
work signature checks in the public Mo view. `review-operator.txt` passes the
existing private-state/authentication/nonmutation driver with the failure view
available; positive empty-view assertions were then added for the final driver run.
`review-smoke3/summary.json` passes: a 20,693-byte failure artifact spans three pages,
all returned continuations work, a changed failure selector refuses, native fetch
publishes mode 0600, and final-stop/snapshot retention remains ids 1 and 15.

Retained failed evidence: `review-fixes-build3.txt` (test fixture attempted a direct
SQLite mutation while the store held its exclusive handle); `review-smoke.txt`
(oversized HTTP request fixture); `review-smoke2.txt` (invalid Text `+` fixture).
The corrected fixture closes/reopens the handle and uses a large pre-state with
small valid requests. The parent authoritative-2 run passed GCC/Clang/sanitizer
unit suites but ended failed for stale driver assertions; its last sanitizer
formatter was interrupted and is not a pass. Final authoritative reruns, rebuilt
reader fuzz/proof/reach and the long kill loop were then parent-owned; their final
results follow.


## Final local verification closure — 2026-11-06

U4 implementation, simplification and actual native-agent code review are complete.
The original review receipt (`review-review.json`) records the earlier Not-ready
snapshot; it is retained unchanged. All six accepted findings listed above were
fixed and a separate read-only reviewer checked their resolution, finding no new
concrete regression. Simplification removed duplicate final artifact verification;
the independent check confirmed that kind/id are published only after complete
validation and checked before file publication. No cross-provider review ran.

- Final GCC, Clang and ASan+UBSan build/unit suites: `startup-compilers-final.txt`,
  last line **`check.sh finished: pass`**. This includes the startup regression fix.
- All-build functional drivers passed, including the complete sanitizer formatter,
  private operator, CLI, taskqueue, golden and recovery drivers. The combined run
  in `authoritative-drivers-final.txt` nevertheless ends **fail** because its final
  timing gate measured job-queue at 0.1132 seconds. This is not reported as a
  passing combined group. A subsequent isolated run failed at 0.1111 seconds
  (`timing-final-alone.txt`). `timing-isolated.txt` overlapped static/model work
  despite its name and is not valid isolated timing evidence.
- Profiling identified unnecessary executable hashing in `mo check`. Identity
  initialization now happens after valid CLI parsing, before execution for every
  command except `check`. A red/green native-main regression is retained in
  `check-startup-red.txt` and `check-startup-green.txt`; an independent reviewer
  found no issue with startup identity for other commands. The SHA-256 debug
  optimization remains. `timing-startup-fixed.txt` passes all twelve samples,
  maximum 0.0746 seconds. The final compiler suites and real capture/fetch smoke
  were rerun after this fix; the lengthy functional drivers were not repeated.
- Static checks and TLC models: `static-models-final.txt`, last line
  **`check.sh finished: pass`**. Includes Capture/Export models and negative
  controls. Final source formatting also passes (`format-final.txt`). No missing
  tool skip is counted as a pass.
- Rebuilt outside-input failure reader fuzz: `fuzz-final.txt`, 1,772,318
  executions in 31 seconds without a crash. This is bounded fuzz evidence.
- Scoped CBMC proofs and reach checks pass for `slice`, `mo_export_page_bounds`,
  `mo_export_schema`, and `export_page_matches` (`proofs-remediated.txt`,
  `reach-remediated.txt`). The slice proof's 32-byte/unconstrained UTF-8 scope
  described above remains; no whole-store C proof is claimed.
- Recovery kill loop: `kill-loop-final.txt`, seed 20261106, 100 kills including
  16 mid-commit kills, 1,669 calls, result pass. Atomic pair crash-boundary tests
  and the clean/unsafe transaction fault matrix also pass in the native suites.
- Final real service/fetch smoke: `startup-smoke-final/summary.json`, pass.
  Fifteen failures retain first/latest ids 1 and 15 through snapshotting and final
  stop. The 20,692-byte artifact uses three ordered pages and publishes mode 0600.
  Changed failure selectors refuse; returned continuations and native fetch work.

These results close local U4 verification. Failure evidence remains beside the
passing evidence; earlier interrupted runs are not passes. U5 offline diagnosis
and U6 evaluation were not implemented. Prospective evaluation approval was not
used as a U4 gate. Pilot evidence remains 30 attempts/29 completed grades, pilot7
interrupted, four uncalibrated variants and no independent baseline. No private
pilot directory was accessed. No commit, push, PR, publication or merge was made.
