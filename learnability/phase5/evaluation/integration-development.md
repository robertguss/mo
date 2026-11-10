# Repair integration development — 2026-11-09

U1 serving-model proof and U2 transparent attribution remain missing. Collection
stays disabled. These are OS-stub and synthetic publication controls, not model
qualification, scored sessions, or independent protected regrading.

Only test_collector.py, publish.py, test_publish.py and this note were edited.
Existing published results were not touched. Parent owns further verification,
independent review and PR work. No C build, full CI, model runs, index writes,
commits, checkouts or subagents were used.

The initial read used the wrong relative path `evaluation/grading-development.md`
and failed; corrected to `learnability/phase5/evaluation/grading-development.md`.
The parent reported prior discovery: 26 tests, 25 passes and one missing
MO_EVALUATION_BINARY error. That is parent evidence, not a run repeated here.

## Collector

Command before and after correction:

```
MO_EVALUATION_BINARY=/opt/mo-phase5/bin/mo PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s learnability/phase5/evaluation -p test_collector.py -v
```

Before: exit 1, one test, two failing subtests, 266.243 seconds. Success expected
status zero but got one; cleanup-failure expected traffic match true but got false.
Output retained at `/tmp/mo-integration-before.log`. Python's traceback displayed
updated source lines because the file was edited during the original run; the
running process retained its original assertions. The corrected run was launched
only after the original exited, avoiding shared account/firewall concurrency.

Corrections require state, independently compiled/persisted activation identity
and source tests to pass, while command origin, traffic, model provenance,
post-activation answer observation and publication remain explicitly false.
No attestation is fabricated. Existing timeout, partial capture, process cleanup,
firewall restoration, private/backend isolation and credential checks remain.
Binary environment lookup now precedes temporary directory allocation.

## Publication

Legacy publication retains its existing path. New dimension records require a
caller-supplied `protected_regrade(directory)` which must independently derive all
seven non-publication dimensions from protected inputs. No default callback or
CLI flag promotes manifest state into independent evidence: CLI publication of
new dimension records refuses until the real regrader is integrated. Synthetic
callbacks in tests establish only this interface, not readiness.

The publisher validates exact boolean dimensions, rejects mixed legacy/new rows,
keeps execution failures as misses, derives publication integrity from scanning,
and recomputes the conjunction without relying on collector `correct` (which is
necessarily false before publication). Public archive extraction reproduces score
aggregation; it does not claim to regrade private state/restart evidence.

Commands:

```
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s learnability/phase5/evaluation -p test_publish.py -v
PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=learnability/phase5/evaluation python3 -m unittest test_collector.Collector.test_missing_binary_does_not_allocate -v
git diff --check
```

Publication: seven tests passed, first in 0.049 seconds and after schema tightening
in 0.056 seconds. Missing-binary regression: one test passed in 0.001 seconds.
Diff whitespace check passed. Controls cover absent independent regrader,
missing dimensions, false provenance, false initial publication status, omitted
slots, development records, credentials including encoded forms, unsafe archives,
symlinks and round-trip aggregation. Existing original-input preservation check
continues to pass.

## Final OS result and cleanup

Corrected OS command: exit 0, one test passed in 274.801 seconds; all three
success/timeout/cleanup-failure subcases completed. Output:

```
test_development_and_timeout (test_collector.Collector.test_development_and_timeout) ... ok

Ran 1 test in 274.801s

OK
```

Raw output is `/tmp/mo-integration-after.log`. The missing-environment test was
added after this process loaded its module and was therefore run separately as
recorded above. Each OS case asserted no remaining account processes and exact
OUTPUT firewall baseline restoration; finally cleanup removed its installation
and private temporary tree. Final `sudo -n pgrep -u mo-phase5-test` returned no
processes, and `sudo -n iptables -S OUTPUT | rg mo-phase5-test` returned no matching
rules (the exact baseline comparison in the test is the stronger firewall check).
Final `git diff --check` passed. All started test processes have exited.

## Independent review corrections

The completed ce-code-review receipt is retained at
`/tmp/compound-engineering-1000/ce-code-review/20261109-followup-1794191820/review.json`.
Its two validated findings were corrected: passing regraded sessions now require
regular stream and proxy files, and real proxy controls use the checkout build
or an explicit MO_EVAL_BINARY override. A new missing-log regression failed for
both files before the fix. Afterward, eight publication and five proxy tests
passed against `/home/exedev/code/mo-m6/build/gcc/debug/mo` in 5.369 seconds.

The retained incomplete-dimensions concern was also corrected. The launch-failure
test first errored with missing task_state; the collector now defaults all four
grading dimensions to false before adding available results. Eleven harness and
eight publication tests then passed in 0.077 seconds. Whitespace checks passed.

GCC completed separately with `check.sh finished: pass`, retained in
`build/evaluation-repair-gcc.txt`. These are partial-repair checks, not a completed
plan or qualification receipt. Positive native-matching coverage, transparent
origin observation, trusted model metadata, the protected regrader implementation
and the fresh cohort remain outstanding. No scored sessions have launched.

## Actual protected capture integration (2026-11-09)

The development collector now launches its existing isolated-account Python entry
under root `strace -u`; frozen client commands and agent shell commands are
unchanged. Before launch it reserves a private nonce and pins the read-only Mo
binary (the harness additionally requires root ownership). After whole-UID
`end_all`, zero-process verification, proxy shutdown and exact OUTPUT firewall
baseline restoration, it seals trace, proxy log and nonce-bound TCP snapshots.
`proxy.match(..., capture=Capture, session_id=..., evaluation_endpoint=...)`
consumes that private capture once, verifies process completeness, joins exact
connection/request occurrences and received responses, and credits authenticated
push executable generations. Dictionaries with an agent-supplied qualification
flag are rejected. Native occurrences retain their existing matcher.
`grade.attestation(..., matched=...)` consumes the collector's reconciliation;
it requires the sole commit and every upload request to have authenticated push
origin before accepting its report. Persisted ordinal reconciliation is unchanged.

Only proxy-endpoint bytes enter HTTP reconstruction. Other process lifecycle
records remain checked, including provider processes. Exact unfinished/resumed
fork pairs establish lineage at fork entry, because actual vfork children can
execute before the parent's syscall returns. This is not a wall-clock or
cross-channel event ordering claim. Unsupported descriptor sharing, truncated
capture, incomplete timeout and unresolved joins fail closed.

Final revision observation still explicitly fails: this integration does not
prove a successful received revision response after commit and before the final
assistant event. No timestamps or channel-local sequence numbers are fabricated.
Model proof remains false, publication remains false, and scored entry remains
disabled pending U1 qualification, independent review and freeze. Protected
regrader integration remains later work.

Retained development failures: /tmp/mo-u23-collector.log (fixture traversal),
/tmp/mo-u23-collector-2.log (writable installed binary rejected), and
/tmp/mo-u23-collector-3.log (vfork ordering rejected). The earlier test fixture
removed its private trees; subsequent controls retain private evidence under
/home/exedev/phase5-test-private-* and never publish raw traces. The focused
first run /tmp/mo-u23-focused.log had 40 tests and one error because the launch
failure mock also intercepted process verification; verification now records
that failure without abandoning cleanup. Focused intermediate results were
43, 45, and 46 passing tests; logs are /tmp/mo-u23-focused-{2,4,5}.log.
No model runs, scored sessions, Git/index/branch/PR operations, subagents, C builds
or full CI were performed. Parent owns authoritative verification and shipping.

Final results:

- Focused origin/identity/observer/join/harness/proxy suite: 47 tests passed,
  4.852 seconds (/tmp/mo-u23-focused-final-2.log). Includes both clients' synthetic
  native duplicate-occurrence positive/negative controls, rejection of agent
  dictionaries, TLS exclusion, and exact vfork resume versus missing-resume cases.
- `python3 -m unittest test_grade -q`: 4 tests passed, 107.300 seconds;
  11 real-service variants inside the suite (/tmp/mo-u23-grade.log). These
  development references and synthetic observation flags are not model proof.
- `MO_EVALUATION_BINARY=/opt/mo-phase5/bin/mo ... test_collector.py -v`:
  2 tests passed, 277.963 seconds (/tmp/mo-u23-collector-4.log), covering actual
  protected success, timeout and injected reset-reporting failure. Success and
  reset-failure each verified six actual Mo push requests and six received
  responses. State, activation identity, source tests, origin and traffic are true;
  observation/final-answer, model and publication remain false. Timeout capture
  is incomplete and traffic is false. All overall results remain misses.
- Private control evidence retained at
  `/home/exedev/phase5-test-private-fmbw8gmn`. Existing publication scanner,
  with both service credentials plus provider secrets and encoded forms,
  passed 17 success, 17 reset-failure and 13 timeout publication-eligible files
  including source trees. Raw traces/identity files were excluded, not sanitized
  and published. This is a scan control, not protected regrading/publication.
- Each case recorded zero UID processes and exact firewall restoration. Final
  `pgrep -U mo-phase5-test` exited 1 with empty output; home mode restored to
  0750. Temporary test install removed. All test processes finished.

During the OS run, final cleanup verification was hardened to distinguish pgrep
exit 1 from errors, and firewall-verification exceptions now remain recorded
cleanup failures. Explicit `shell_origins` records were also added to the matcher.
The final focused run covers those edits; the OS run exercised the same success
path before those narrow error/reporting edits. No second OS claim is implied.
Production capture requires the pinned root-owned Mo executable to be read-only
(the isolated control uses 0555); existing setup's 0755 install is rejected.
Remaining limitations are bounded scalar TCP HTTP/1 Content-Length, conservative
shared-descriptor/process-reuse rejection, absent trusted cross-channel final
observation ordering, U1, independent review/freeze and protected regrading.

## Private replay retention prerequisite

New captures retain `replay-private/` before one-shot reconciliation and before
stopped grading mutates data. This directory is explicitly excluded from
publication (the publisher's existing allowlist does not include it). Raw files
may contain secrets. Directories are 0700 and payloads/manifests 0600.

`Bundle(root, Capture)` requires a sealed, unconsumed actual collector Capture.
It copies exact executable bytes and sealed trace/proxy/stdout/TCP inputs,
retaining original pins, nonce, session, loader bytes and verified mapping pins.
`add(role, path)` and `tree(role, path)` retain protected inputs;
`finish(launch_facts)` seals an exact inventory. The initial limits were 128 MiB and 4096 files; the current byte ceiling is
256 GiB (not a full-session capacity qualification);
overflow fails retention, never weakens path policy. No consumed capture reopens.
`verify(root, trusted_session_id, trusted_session_uid)` reads the private manifest
itself and checks ownership, ancestor policy, exact inventory and original retained
inode/metadata/hash pins. It returns metadata, never a grading qualification.
Collector UID and trusted session binding remain the trust boundary.

Harness inputs include expected fixture, spec, prompt, original source, context,
captured candidate, clean stopped data, raw live RPC values, launch facts and
endpoint. Tree member lists map relative names to flat retained payload roles.
Empty directories are not reproduced. Mapping facts retain original identity
pins; mapping library bytes other than the ELF loader are not copied. The future
regrader must provide compatible protected libraries and validate those facts;
it must copy data into a separate private replay workspace before mutation.
The bundle is sealed before initial grading: grading output is deliberately not
an independent proof. A retention failure remains a collector cleanup failure.

Red: missing replay_bundle import, recorded in build/replay-red.txt. Green:
60 focused replay/harness/identity tests in build/replay-green.txt, including real
small ELF capture, roundtrip, tampering, missing/symlink/extra/writable inputs,
wrong session and refused second consumption. Source assertions check retention
ordering. Heavy collector, C builds, model runs and scored sessions were skipped
as requested. Existing evidence was read only for the two scanner inventories;
no original results changed. Scanner passed the four changed eligible text files
with both inventories before this note was appended; parent should rescan final
artifacts. All test processes exited.

Remaining work: implement independent replay checks against verified inputs,
validate replay runtime mapping compatibility, integrate protected publication,
and run the full collector controls. Model qualification remains false and
scored collection remains disabled. No launcher or publication gate changed.

## Protected replay implementation work

Added `protected_regrade.regrade(root, session_id=..., session_uid=...,
workspace_parent=...)` and `callback(bindings, workspace_parent)` for the existing
publisher callback. Bindings must come from the collector, outside incoming
publication manifests. Replay verifies retained inputs, reconstructs private
source/state trees, runs source tests and migration compilation, restarts the
copied state, and derives diagnostics from raw live values and captured stdout.
The matcher accepts a distinct verified ReplayReceipt without reopening Capture.
Model provenance remains false; publication integrity belongs to the publisher.
Compatible original protected system loader and mapped libraries are required;
the retained executable can run after its original installation is removed.

Verification: existing bundle suite passed 31 tests. Focused replay, bundle,
identity, proxy, publication and harness suite passed 113 tests, then 114 tests
in 6.009 seconds after adding retained-executable relocation coverage. Counts
include inherited identity/bundle cases. No test-first red run was recorded.
Controls use actual small retained files and ELF execution, but do not yet prove
an end-to-end Mo regrade or manifest-forgery resistance through publication.
Full evaluation discovery was accidentally started instead of only the focused
suite; its separate result must not be confused with collector qualification.
No new model sessions, builds, Git mutations or original evidence edits occurred.
Original 0/4 and disabled scored entry remain unchanged. Parent still owns the
real replay positive, independent review and full OS verification. This unit is
not ready for authoritative publication use until that validation is complete.

Accidental discovery finished: 177 tests in 111.570 seconds, one error and no
reported skips. The collector test failed before launch because
MO_EVALUATION_BINARY was unset. Output is retained in `learnability/phase5/results/development-failures/regrade-tests.txt`.
All started test processes finished. Final five edited eligible files passed the
publication scanner with both private credential inventories after this edit.

## Real protected replay development validation, 2026-11-09

Declared and attempted two fresh installed-stub DEVELOPMENT success collector
cases, separately: initial attempt and one corrective rerun. No model calls,
scored sessions, runtime builds, Git operations, or full five-case collector run.
The class used was `test_collector.Collector`, with only `cases=('success',)` and
`MO_EVALUATION_BINARY=/home/exedev/code/mo-m6/build/gcc/debug/mo`.

Before collection, losslessly gzipped only the five completed v253rwwd raw traces.
Each adjacent `trace-storage.json` records original size and SHA256; decompressed
SHA256 was verified before removing the uncompressed copy. No evidence discarded.
Initial free space was 231 MiB, after compression 720 MiB.

Attempt 1: `/home/exedev/phase5-test-private-3a1g4p1v`, UTC 11:53:02–11:54:43,
101.074 seconds total, client capture 3.113 seconds. Its 145,526,721-byte trace
exceeded the old 128 MiB retention bound. Retention rejected; state, activation
identity and source tests passed, command origin, traffic and final observation
failed downstream. Failure retained in `build/real-replay-attempt-1.txt`.
The bounded aggregate retention ceiling is now 256 GiB, based on the observed
approximately 47 MB/s over the approved 3600-second session with headroom.
This ceiling is not a disk-space reservation or proof of full-session scalability;
the existing in-memory parser still needs resource review before scored use.

Corrective attempt 2: `/home/exedev/phase5-test-private-qbi70qx0`, UTC
11:55:20–11:57:05, 104.450 seconds total, capture 3.056 seconds. Collector success
assertions passed. Retained mapping parser now receives TCP snapshots and the
actual evaluation endpoint; this bundle retained 13 mapping pins and 21 files.
After installation cleanup, actual protected replay used the collector output
path as session binding, the OS isolated UID, and a separate private workspace.
Replay ran UTC 11:57:15–11:58:26 (71.555 seconds). Recomputed task_state,
activation_identity, source_tests, command_origin, traffic_completeness and
final_answer were true; model_provenance and overall correct were false.
Evidence: root `recomputed.json`, `build/real-regrade-attempt-1.txt`.
The replay itself verified unchanged original bundle bytes before/after.

A subsequent negative-control attempt was defective: its script hard-linked
retained files before tampering with copy entries. Hard links preserved bytes
but changed original inode ctime, invalidating original retained pins. The script's
byte-hash assertion missed this. Do not treat `build/real-publication-negatives.txt`
as valid individual tamper controls or retrofit the original manifest. The
original bundle is now unsuitable for further pinned replay. This mistake occurred
after the successful real regrade. All artifacts and defective script are retained.
A fresh collector is needed for repeatable replay/publication validation; available
disk cannot accommodate another full raw trace plus retained copy without further
explicitly scoped storage work. This is unfinished validation, not an unavailable
tool capability. Parent's final full collector remains unrun.

Runtime trust regression red: 42 tests, one failure, in
`build/real-replay-trust-red.txt`. Runtime validity now gates execution and receipt
consumption; consumption also checks loader pins and endpoint mismatch. Additional
synthetic tests reject false runtime validity, mismatched endpoint/loader, and
publication manifest forgeries through the actual callback with an incomplete
protected fixture. Existing synthetic tamper cases cover retained files, sessions,
and missing inputs. No synthetic fixture is claimed as a real FINAL session.
Final focused output: `build/real-replay-focused-final-3.txt`.
Cleanup evidence: `build/real-replay-cleanup-check.txt` verifies both session users
have zero processes, exact `-P OUTPUT ACCEPT`, operator home 0750, and both fake
credentials byte-identical to the retained snapshot. Final scanner output is
`build/real-replay-scan.txt`, run after this append against both new private
inventories. Remaining review: independent trust-contract review, valid real
publication tamper controls, scalable retention, and parent's full collector.
Original official 0/4 remains unchanged; models remain unqualified and scored
collection remains disabled.

## Repeatable development replay control (2026-11-09)

`MO_REAL_REPLAY=1 PYTHONDONTWRITEBYTECODE=1 python3
learnability/phase5/evaluation/test_real_replay_development.py` is the opt-in
executable instrument. It declares one success collector, uses the existing
GCC binary, retains evidence, and never retries. This run retained
`/home/exedev/phase5-test-private-4lbb9izy`. Collector assertions passed in
106.139 seconds; after installation removal two actual protected callback
replays passed in 71.436 and 71.339 seconds. Both computed the same six true
non-model dimensions, false model provenance and false aggregate credit.
Every retained file, including the manifest, matched device/inode, mode,
owner/group, size, ctime, mtime and SHA256 before/after each replay. Original
public manifest bytes were independently backed up and restored. No hard links,
chmod, renames or writes touched this pristine bundle.

Both incoming correct=true and correct=false public manifests claimed all true
dimensions. The actual callback ignored those claims and actual `public_rows`
aggregation refused credit. This is explicitly a synthetic publication input
fixture: no FINAL dataset, archive publication or scored counter was created.
This checks the actual callback and credit aggregation, not the full four-slot
publisher end to end. Existing publisher unit controls cover that separate path.
Development collection remains guarded; model/publication qualification is false.

The process exit was 1: unittest also discovered an imported Collector class,
whose unrequested test errored on missing MO_EVALUATION_BINARY before allocating
or launching anything. The declared real test itself passed. The instrument now
imports the module instead, preventing that extra discovery. No collector rerun
was made. Raw output is `build/repeatable-real-replay.txt`; precise measurements
and pins are `repeatable-control.json` and `control-original-pins.json` under the
private root. The focused suite passed 100 tests in 0.930 seconds
(`build/repeatable-focused-final.txt`). Fresh small synthetic tamper controls first
verify valid baselines. Bypassing verification causes five assertion failures
(`build/repeatable-tamper-red-2.txt`). The first mutation experiment exposed
missing finally cleanup, producing one failure plus four incidental errors;
its failure is retained in `build/repeatable-tamper-red.txt` and cleanup corrected.
The earlier incomplete-bundle/duplicate-slot forgery test was removed.

Only completed 3a1g4p1v raw trace and qbi70qx0 raw/retained traces were compressed.
Decompressed SHA256 was checked before each raw unlink; adjacent private-root
`replay-storage-receipt.json` files record hashes and sizes. Original manifests
and previous failure notes remain. These old pinned bundles are not reusable.

The new trace is 145,526,867 bytes; bundle is 153,996,843 bytes. Process peak RSS
was 755,368 KiB. The 256 GiB aggregate limit is an unqualified resource ceiling,
not demonstrated hour capacity. `read_bytes` plus whole-trace parsing makes a
full hour impractical in RAM without further resource work. No bound was raised
here. Independent trust review, resource qualification and the parent's full
collector controls remain required; parent owns authoritative verification/PR.

Final cleanup verified all four session/test accounts have zero processes,
exact OUTPUT ACCEPT, operator home 0750, fake credentials equal snapshot,
all pristine pins unchanged and original public bytes restored. An initial
cleanup command used nonexistent account `mo-phase5`; corrected to the actual
account names. A broad scanner attempt included unchanged `test_collector.py`,
which intentionally contains the fake credential fixture literals and was
rejected. Final scanning is scoped to this worker's three changed files plus
the fresh publication allowlist, against both private credential inventories.

## Parent fixes for independent replay review

Review /tmp/compound-engineering-1000/ce-code-review/replay-5tdz6mer found three confirmed defects. Publication now requires dimensions for every new cohort input, so omission cannot select manifest-only legacy credit. Historical artifacts remain unchanged; the default publisher no longer accepts legacy manifests. New captures retain immutable queue-tests.mo, initial grading uses that retained file when retention succeeds, and replay requires it instead of reading current checkout tests. Older bundles without the retained test file cannot qualify through this replay contract. Missing or invalid migration reports now return failed compilation identity diagnostics while source, traffic and state checks continue; nonzero candidate compilation also returns a miss.

Red logs: build/publication-downgrade-red.txt (nine tests, one confirmed omission bypass); build/replay-immutable-red.txt (missing retained-test argument); build/replay-missing-report-red.txt (missing diagnostic helper). Final focused verification: 104 tests passed in 1.324 seconds, build/replay-review-fixes-focused.txt. Whitespace check passed. These focused controls do not replace real-service no-activation/miss verification or independent re-review. No models, scored sessions, collector runs or original evidence edits occurred. Resource capacity and full collector/publication controls remain unresolved.

## Storage-limited integration follow-up (2026-11-09)

Initial df reported 170 MiB available on the 246 GiB filesystem. Declared and
launched zero new OS collector cases; no retries, compression, builds, models,
Git operations, or original evidence changes. Added
`test_real_source_development.py`: two small real-GCC-Mo controls exercising
retained immutable tests with an absent and then invalid alternate checkout
fixture, and real compilation failure plus absent-report identity diagnostics.
The alternate checkout is a test-local ROOT override, not a repository edit.
Retained test bytes and full metadata pins are compared before/after.
These are source_precheck/compile_identity integration controls, not actual
sealed regrade proof. No production fixes or regression-red claim are made.

Protected regrade tests: 46 passed; publisher tests: 9 passed
(`build/integration-small-focused.txt`). Publisher controls remain explicitly
synthetic, including their model=true callbacks; they prove neither real model
provenance nor a real failed-session regrade. Cleanup passed for both isolated
users, OUTPUT ACCEPT, home 0750 and both credential snapshots
(`build/integration-small-cleanup.txt`). Read-only verification of pristine
4lbb9izy pins matched its saved baseline. This follow-up never touched its files.

Remaining gap: fresh sealed success with required immutable_tests and fresh
legitimate no-accepted-upgrade session, complete boolean dimensions through
actual regrade, and actual regrade compilation-failure coverage. Stub noop
configuration remains to be implemented when those collector controls run.
No fresh publication allowlist exists because no collector launched. Independent
re-review may inspect these limited controls, but the requested integration gate
and full collector are NOT ready. Model provenance remains false and scoring
disabled. The 256 GiB ceiling and hour capacity remain unqualified.

Real-Mo controls passed: 2 tests in 133.345 seconds, including both retained-test
executions (`build/real-source-development.txt`). Total focused tests in this
follow-up: 57 passed. All launched processes finished. Final eligible-file scan
uses the existing two private service/provider inventories and excludes the
unchanged collector's intentional fake credential literals; output is retained
in `build/integration-small-scan.txt`.

Parent storage recovery: losslessly compressed 150 generated GCC/Clang debug unit-test executables in this checkout, verifying each decompressed SHA-256 before removing the original executable. Recovered 653,897,488 bytes; free space rose to 787 MiB. build/replay-space-recovery.json retains paths, hashes, sizes and modes. These generated executables must be restored or rebuilt before compiler checks. No source, logs or captured evidence changed. This enables the two pending declared development collector controls without weakening retention limits.

## Two sealed controls after storage recovery

Declared exactly TWO fresh DEVELOPMENT collectors before launch: success and
legitimate noop, seed 1 each, no retries. Runnable artifact:
`MO_SEALED_CONTROLS=1 PYTHONDONTWRITEBYTECODE=1 python3 learnability/phase5/evaluation/test_sealed_controls_development.py`.
Existing GCC Mo only; no builds, models, scored sessions, Git or subagents.
Evidence remains at `/home/exedev/phase5-test-private-_r18mbtz`.
Terminal log: `build/sealed-controls-development.txt`, exit 0, one test in
324.972s. Both collectors completed in 185.323s, with no assertion errors.
Client capture durations were 3.014490s and 0.329580s.

Success replay took 70.633s: all six non-model dimensions true, model false.
Noop replay took 68.746s: source_tests and traffic_completeness true; task_state,
activation_identity, command_origin, final_answer and model_provenance false.
Both returned complete booleans and correct=false after installation removal.
Noop diagnostics explicitly report missing accepted migration, wrong live/restart
revision, identity/origin/answer misses. Old queue source passes the immutable
queue tests; that does not establish the requested revision upgrade. No fake
accepted report was created. The noop uses the ordinary mo-state read tool.

Success trace: 145,534,029 bytes; retained directory 154,015,977 bytes / 23 files.
Noop trace: 11,254,860 bytes; retained directory 19,574,001 bytes / 22 files.
Counts include each manifest. Both retain immutable_tests. Replays override only
test-local fixture ROOT to an absent path; before/after full file pins match.
All evidence and failures retained; no hardlinks, old evidence edits or repinning.

Before the behavior fix, real noop collector grading recorded
`cleanup grading: ValueError`; independent replay already returned an ordinary
miss correctly. Narrow harness correction handles missing migration selector and
nonzero compilation as identity misses and continues state grading. Trust errors
are not caught. Focused missing-report regression passes (1 test, 0.001s).
Related harness/regrade/publication suite: 71 passed in 0.673s; earlier regrade/
publication check: 55 passed in 0.919s. Real compilation helper control: 1 passed
(`build/sealed-compilation-control.txt`). The added noop diagnostic/source/traffic
assertions were written after the running test loaded and checked separately
against its actual retained verdict. No collector rerun after the harness fix.

Remaining gap: compilation failure through the entire sealed regrader (only the
real compilation helper is exercised); independent re-review and parent's full
checks. No additional collector or retrofitted bundle was used to fill that gap.
Resource hour capacity is unqualified: roughly 145 MB for 3 seconds, whole-trace
RAM parsing, and a 256 GiB ceiling remain unresolved. Official original 0/4 is
unchanged, models false, scored collection disabled.

Changed files: stub_client.py (opt-in noop, default five paths unchanged),
test_collector.py (noop configuration/assertions and missing-report regression),
harness.py (ordinary missing-report miss), test_sealed_controls_development.py,
and this note. Final cleanup/pins/credential/public-allowlist scanner receipt:
`build/sealed-final-check.txt`. Scanner includes eligible collector text while
excluding its unchanged intentional fake-credential fixture line, and uses all
four private service/provider inventories. Parent owns review/full checks/PR.

Incomplete re-review returned no completed independent reviewer batch or full receipt. Its development-classification concern was reproduced by actual replay of the existing sealed noop bundle (_r18mbtz): build/replay-classification-red.txt returned SESSION and failed the required DEVELOPMENT assertion. Replay now passes the verified collector development flag to stopped_grade. A second actual replay passed that assertion, returned the complete diagnostic miss, and verified all retained pins unchanged (build/replay-classification-green.txt). No collector or model launched. The sealed-control runner now asserts the classification. Related 71 focused tests passed in 0.964 seconds (build/replay-classification-focused.txt); whitespace passed. A complete independent re-review remains required.

## Complete replay re-review fixes in progress

Full independent receipt /tmp/compound-engineering-1000/ce-code-review/replay-fresh-4rgs9av4 confirmed incomplete-capture retention, publication classification loss and source-test timeout handling defects. Parent fixed classification propagation: callback now returns the protected full verdict, including evidence class and slot/client; publisher requires FINAL and matching slot/client before accepting dimensions. It no longer loses DEVELOPMENT classification at the callback boundary. Source_precheck now converts only TimeoutExpired into an explicit timed-out test result (124); collector and replay record that reason and continue independent checks. Integrity exceptions remain errors.

Regression red logs: build/protected-class-red.txt and build/replay-source-timeout-red.txt. Focused final run: 75 tests passed in 0.906 seconds, build/replay-latest-fixes-focused.txt. Whitespace passed. No actual model or collector sessions launched. Full-verdict callback callers were updated. Incomplete capture records remain the next implementation task; full collector/review/resource qualification remain pending.

## Protected incomplete retention (review finding 1)

The collector now retains launched incomplete sessions as version 2
`collector-partial` records. `PartialBundle` is collector retention, never a sealed
Capture and never a way to reopen a consumed Capture. Version 1 complete receipt
and runtime identity behavior remain unchanged. Protected launch facts bind session,
UID, slot, client, DEVELOPMENT/FINAL class, timeout and cleanup results; outer flags
cannot change replay class. An availability list distinguishes absent/unsafe inputs
from malformed, missing or tampered receipts, which reject.

Mutable candidate/state/trace inputs are copied only after zero isolated UID
processes, restored firewall and terminal service verification. Missing or unsafe
inputs produce false dimensions with reasons. Partial regrade reconstructs private
copies, runs available source tests and state/restart checks, and attempts independent
compilation when an accepted protected report exists. It never credits command origin,
traffic completeness or final observation from incomplete capture. The binary and
resolved loader are retained before installation removal. No global source checkout
supplies replay tests. Fixture failure before launch remains a non-launched fixture
failure; launched misses retain their protected publication class and denominator role.

Test-first: initial new tests had three missing-API errors (29 executions including
inherited identity tests). Additional real-private-file controls exposed the resolved
loader-path bug and were corrected. Final focused harness/regrade/publish/identity/
bundle/partial suite: 147 executions passed in 1.682 seconds, including inherited
identity executions. Controls cover explicit absence, missing/malformed/corrupted
records, forged outer class/flags and independent source success/failure using retained
private inputs and real true/false executables (these last two are plumbing controls,
not Mo compilation qualification).

Two separately declared DEVELOPMENT timeout collectors were launched, each with a
three-second existing sleeping stub, no models and no builds. First attempt:
`phase5-test-private-cp_zd2th`, 105.506 seconds total, retained failure: loader symlink
rejected and no finished partial manifest. Corrective attempt after resolved-loader
fix: `phase5-test-private-ne7nzzch`, 101.091 seconds total, collector assertions passed.
Both attempts and their logs remain retained; no evidence was patched or repinned.
Logs: `build/partial-control.txt`, `build/partial-control-corrective.txt`.

Bounds: no cleanup-reporting-failure collector was added; default five collector
cases remain intact. Full-hour resource capacity, real FINAL cohort publication,
state-positive partial replay, compilation-failure partial replay, and complete control
coverage remain unqualified. Parent owns full GCC, default five collectors, full
independent review and PR. Model provenance stays false; scored entry stays disabled;
original official 0/4 is unchanged. No ceiling was raised or old evidence modified.

Corrective actual regrade completed in 69.490 seconds: source_tests true; task_state,
activation_identity, command_origin, final_answer, traffic_completeness and
model_provenance false; protected DEVELOPMENT slot 1/codex, overall false. State
check lacked sufficient retained live observations; this is an explicit diagnostic
miss, not state coverage. Pins before/after were identical. Final code additionally
attempts compilation from an available protected report; that later branch was
included in the passing focused-suite revision but was not exercised by this
no-report timeout. Final post-note scan
and cleanup receipt: `build/partial-final-check.txt`.

Final availability correction: an absent revision endpoint is an observed null
revision under the existing observation contract, so jobs plus mo-state suffice to
attempt independent state checks. Same retained bundle, no new collector: current-code
regrade took 69.554 seconds, source_tests true and all other dimensions false, with
actual live/restart wrong revision and stored_revision reasons and no accepted
migration report. New focused plumbing control verifies state checking continues
without a revision key; state-positive/failure returns there are mocked, not real Mo
state qualification. Current-code replay preserved every bundle pin. Final logs are
`build/partial-current-regrade.txt` and `build/partial-all-focused.txt`. The final
scanner below was rerun after this append; all collector and replay processes ended.

## Parent full collector after streaming integration

Declared exactly five DEVELOPMENT cases in one invocation: native success, curl, urllib, timeout and injected cleanup-reporting failure; no retries or model calls. All passed: three unittest methods in 658.500 seconds, build/stream-full-collector.txt. Private evidence /home/exedev/phase5-test-private-uy6pf3c_. Gzip capture, complete and partial retention, missing-report handling and source-test timeout implementation were present in this run. Synthetic cleanup failure remains an overall miss; model and publication dimensions remain false.

Parent independently verified both isolated accounts empty, exact OUTPUT ACCEPT, home0750, and both fake credential files matching the retained snapshot. All 82 publication-eligible files passed the existing scanner against service/provider inventories; raw compressed captures and replay bundles excluded. This is the full development collector check, not frozen-client qualification or a scored cohort. Lifecycle review, compilation-failure sealed replay, long-run resource evidence and actual-model provenance remain pending.

Repository-required GCC completed: check.sh finished: pass, build/stream-final-gcc.txt. All requested checks in that invocation completed; no additional compiler/sanitizer/fuzz/driver/static groups were invoked for this Python/documentation-only follow-up. Generated sanitizer test executables remain in a verified restorable archive; GCC executables were restored before checking. No checks remain running.

## Declared sealed compilation-failure control

Exactly ONE new DEVELOPMENT collector, compile-failure, seed 1, no retries.
Existing build/gcc/debug/mo only. Stub pushes the accepted migration and observes
revision 1, then appends invalid syntax to local job-queue.mo before its final
answer. Captured source stays broken; committed service stays correct. Expected
independent verdict: task_state, command_origin, traffic_completeness and
final_answer true; activation_identity, source_tests, model_provenance and
overall false, with candidate upgrade compilation failed. Retained immutable
tests and fresh replay workspace are required; all original pins stay unchanged.
Default five collectors remain unchanged. No model/scored runs or readiness claim.

Actual single collector completed in 77.885s with zero collector errors/failures,
six verified push requests. Fresh protected replay completed in 4.225s, returned
DEVELOPMENT slot 1/codex and exactly the predicted seven booleans; reasons were
activation identity mismatch and candidate upgrade compilation failed. Bundle:
23 files, 27,250,155 bytes, immutable_tests retained, installed binary removed,
all original pins unchanged. Evidence: /home/exedev/phase5-test-private-bos131pz.
Failed captured queue source remains intact; serve copy lacks the invalid suffix.

Runner red retained at build/compile-failure-development.txt: after successful
actual replay, assertion code used nonexistent replay.publish (82.175s total).
Fixed the test import to publish; independently checked every corrected verdict
assertion against the saved actual verdict, build/compile-failure-verdict-check.txt.
No automatic retry or second collector. No production changes were needed.
74 focused regrader/harness/publish/collector unit tests passed in 0.712s,
build/compile-failure-focused.txt. New runner is executable and opt-in through
MO_COMPILE_FAILURE=1; imports the collector module and preserves subtest failures.

Final receipt build/compile-failure-final-check.txt verifies both isolated UIDs
empty, OUTPUT ACCEPT, home0750, snapshot credentials restored, private bundle
0700/0600 and unchanged pins. After this note, 21 changed eligible/publication
files scanned against both new and parent-full-collector service/provider
inventories; only the preexisting intentional fake-credential fixture line is
excluded from collector text. Raw evidence stays private. No old bundles edited,
copied, hardlinked, repinned or decompressed. All launched processes finished.
Original official 0/4 unchanged; scoring disabled. Full-hour capacity and model
proof remain pending; parent owns authoritative verification and independent review.

### Integrated review lifecycle corrections (2026-11-09)

The independent receipt at
`/tmp/compound-engineering-1000/ce-code-review/integrated-us7qkzbp`
reported four P1 failure paths: unverified cleanup followed by destructive source
reset; state retention before verified service exit; a failed full bundle
preventing partial fallback; and an unbounded tracer wait when the compressor
exited before opening its FIFO. No incidence was measured. Its coverage excludes
full origin internals and actual-model/full-duration qualification.

Corrections require verified service exit and captured source before full sealing
or stopped grading. Cleanup verification is retried. Launched work whose source
was not captured is preserved at its original path, and a private
`retention-hold.json` blocks setup and future collection from resetting it.
Destructive reset is withheld in that failure case; credential restoration is
therefore not claimed for a held session. Recovery must first establish process
quiescence and preserve that work before clearing the hold. This is an operator
recovery condition, not an added rule for evaluated agents.

A full bundle is considered complete only after finish succeeds. Failed attempts
remain private; fallback uses `replay-partial-private` when the full attempt
already occupies `replay-private`. The protected callback recognizes this fixed
private fallback path, independently of public manifest flags. Individual copy
and tree failures roll back only unfinished retention copies, leaving originals
and previously retained roles intact; optional partial trees can record an
unavailable role without discarding the receipt. Full or partial retention must
finish before collector grading runs.

The tracer monitors compressor exit and uses bounded process-group termination
and forced kill, including when the session cleanup callback raises. A one-second
exit grace handles compressor EOF preceding tracer exit. The grace was added
while the declared real collector control was already running; that process had
loaded the preceding implementation. Focused tests exercise the final code;
there is no claim that the real collector used that later grace adjustment.

New controls were written after the first implementation edits; no pre-fix red
claim is made. Actual compressor-exit and cleanup-exception controls leave all
spawned processes terminal. Mocked collector controls cover live-service refusal,
source-copy failure, failed process verification, and constructor/population/
finish failures. A real-file limit control verifies rollback, unchanged originals
and a valid partial manifest. The first regression run had 148 tests and two
errors: a temporary fixture under world-writable `/tmp` was correctly rejected,
and service launch failure exposed a missing process attribute. Both were fixed;
`build/lifecycle-regressions.txt` preserves the failed run. The corrected run
passed 148 tests; with the protected callback fallback control added, 149 passed
in 5.050s (`build/lifecycle-regressions-final.txt`).

Declared before execution: exactly two DEVELOPMENT collectors, success and
timeout, no retries or model calls, followed by one timeout partial replay.
Command and result log: `build/lifecycle-development-control.py` and
`build/lifecycle-development-results.txt`. Results are recorded below once the
process finishes. These changes do not qualify the original 0/4, frozen model
provenance, full-hour capacity, FINAL publication, or release.

The two declared collectors completed without assertion failures, with private
artifacts at `/home/exedev/phase5-test-private-eyrckom0`. Success passed task
state, activation identity, command origin, final answer, source tests and traffic;
model and publication remained false. Timeout passed source tests and failed
all other dimensions. The subsequent actual protected timeout replay completed,
retained DEVELOPMENT classification, returned complete false credit for origin,
traffic, final answer and model, and preserved every bundle pin. The result log
contains replay timing and all dimensions; `lifecycle-timeout-regrade.json` is
private. No replacement collector or model session ran.

Final cleanup verified zero processes for both isolated accounts, OUTPUT ACCEPT,
home mode 0750, and both fake credentials exactly matching the retained snapshot.
The 31 eligible public files from these two sessions passed scanning against both
credential inventories. Seven changed source/documentation files also passed a
scan. No raw capture or replay bundle was published. The prior GCC result remains
`build/stream-final-gcc.txt`: `check.sh finished: pass`; this correction changed
Python and development documentation, not C or embedded guide files. A fresh
independent review of these lifecycle corrections remains required.

### Retention exhaustion follow-up (2026-11-09)

Review `lifecycle-366ky6p0` found two further paths: failure to write a retention
hold could permit a later reset, and an optional trace/file copy exceeding the
remaining budget could prevent partial-manifest completion. The startup guard
now refuses an existing work path independently of any hold file, including
symlinks, and propagates inspection errors. The hold is explanatory metadata;
its successful creation is no longer necessary to protect existing work.
Optional artifact copies now record unavailable roles and continue to finish the
partial receipt. Required input or manifest writes can still fail if the storage
itself cannot accept them; no claim is made that disk exhaustion permits writing
new evidence. Original inputs are preserved.

The first preimplementation test run retained in
`build/retention-exhaustion-red.txt` had three errors: the optional-copy helper
was absent (two budget cases), and the missing guard reached an incomplete test
layout. The layout was corrected and the old guard was explicitly restored in
memory for a baseline control: it failed with `ValueError not raised`, while host
setup remained mocked (`build/retention-hold-baseline-control.txt`). The final
focused suite passed 153 tests in 5.470s. Controls cover marker-write failure,
byte and file exhaustion after retaining a tree, real receipt verification and
protected callback replay. Initial focused fixtures remain separate from agent
evaluation sessions.

Declared exactly one further DEVELOPMENT collector, with a full-bundle
constructor failure injected after creating its directory, followed by one real
partial callback replay. There are no retries or model calls. Unlike earlier
mocked fallback tests, this uses the actual PartialBundle, verifier and regrader.
The command is `build/retention-fallback-control.py`; results are retained in
`build/retention-fallback-results.txt`. This running process loaded the existing-
work guard before its later lstat-only error-propagation refinement; the final
focused suite must be rerun for that refinement. No final qualification is implied.

Final guard verification: 154 focused tests passed in 4.846s, including denied
path inspection and a dangling work symlink (`build/retention-exhaustion-final-guard.txt`).
The one declared collector completed in 147.206s and retained a genuine fallback
at `/home/exedev/phase5-test-private-pgslqts6/run-retention-failure/replay-partial-private`.
The real callback replay completed, but the runner exited 1 because its assertion
incorrectly required activation identity to be false. This failed assumption is
preserved in `build/retention-fallback-results.txt`: retained source, accepted
report and restarted state can independently establish identity without command
attribution. The returned verdict has state, activation identity and source tests
true; origin, final answer, traffic and model false; DEVELOPMENT and overall false.
No scoring gate changed.

The saved actual verdict was validated with the corrected expectation without
rerunning collection or replay. The real verifier subsequently confirmed all
retained file pins. The original in-memory before/after pin assertion was after
the failed assertion and did not execute; it is not claimed as passed. Replay
elapsed time was likewise not printed. The initial result log remains intact;
`build/retention-fallback-saved-validation.txt` records the separate validation.
Zero processes for both isolated users, OUTPUT ACCEPT, home 0750 and exact
snapshot credentials were verified. Seventeen eligible public files passed both
credential inventories. All started processes finished. These two review fixes
have focused verification; no new full independent review receipt is claimed.

## Follow-up capture review and repairs

Independent read-only review of 4dffd7e2..ed7fb31c found eight issues: ordinary
thread/fork/vector traffic was rejected; anonymous executable mappings crashed
full retention; descendants extended a normal client exit into a timeout; command
origin still required push-looking shell text; failed partial state retention
allowed mutation of the only state; absent timeout facts became false; publication
could combine launch facts from another collection; and a historical failed-test
log was misplaced while the documented retention ceiling was stale. These are
readiness defects, not a new scored result. The reviewer ran only an offline parser
control, not a client session or whole suite.

Repairs preserve unknown/JIT mappings without granting pinned native identity,
use protected transfer attribution rather than shell display text, require retained
state before mutating grading, preserve unknown timeout as null and known timeout
through tracer exceptions, and bind publication to protected collection ID/start
time plus protected clean-execution facts. The failed-test log moved unchanged to
results/development-failures/regrade-tests.txt; it is intentional retained evidence.

Thread groups now share descriptor state and a process-image identity. A fork
that never transfers on an inherited socket no longer poisons its parent's
receipts; actual transfers from different process images still fail closed.
Close-on-exec sockets are removed at exec. Explicit vector buffers are decoded
only through the successful byte count; truncated vectors, ancillary data,
unsupported thread flags and kernel asynchronous transfers remain refusals.
The compressor observes the original tracee's terminal record in its private
stream and triggers descendant cleanup without waiting for the session deadline.
A normal-exit/background-child control passed with complete terminal accounting.

The first three new regression controls failed as expected for command-text and
state-retention bugs; the initial JIT fixture accidentally contained no request.
The corrected JIT fixture reproduced the TypeError, then passed after the fix.
Logs: build/review-retention-red.txt, build/review-jit-corrected-red.txt and
build/review-retention-green.txt (76 tests passed). Root lifetime/storage controls
passed 47 tests (build/review-exit-controls.txt). Initial vector decoding mishandled
the `->` inside TCP annotations; corrected and retained both failing runs. The
55-test parser/identity/descriptor/integrity/join/observation/storage suite passed
in build/review-thread-green.txt. Publication/replay controls passed 91 tests in
build/review-publication-controls.txt. These overlap and are not additive totals.

One bounded Node keepalive HTTP/spawn/vector development control was declared,
with no model calls and a 20-second deadline. It failed before parsing because a
relative trace directory was interpreted from the compressor's working directory.
TraceRun now resolves its directory. The corrected control also failed: this Node
build successfully invokes io_uring_setup and io_uring_enter, which the decoder
rejects. Both captures and failures remain in build/review-node-control*; raw
traces contain environment values and are private, never publication inputs.
This is an unresolved supported-transport boundary, not evidence of transparent
Node or pinned Claude/Codex qualification. No scored runs were started. Further
work needs a capture design that accounts for these transfers or a demonstrated
supported client configuration before a new readiness claim.

The combined focused suite passed 237 tests in 114.874 seconds
(build/review-final-focused.txt), including real-service grading controls.
A follow-up partial-schema check requires the timeout field explicitly while
allowing null for unknown; its focused tests passed (build/review-timeout-schema.txt).
Exactly one separate-account DEVELOPMENT success collector is then run using the
current GCC binary, no model calls, with the existing 60-second client deadline.
Expected: six non-model dimensions pass, model/publication remain false, and
account processes/firewall restore. Result log: build/review-real-collector.txt.

The separate-account success collector passed in 153.047 seconds. Protected
replay after removal of the temporary installation passed the same six non-model
dimensions in 69.964 seconds, and every retained pin compared equal before/after.
Private root: /home/exedev/phase5-test-private-wa6auo32. Model and publication
qualification remained false. Eligible outputs and changed files (35 files)
scanned clean against four credential inventories.

The independent parser/lifetime re-review identified three medium and two low
issues. Final corrections reject duplicate resolved endpoints regardless of which
process transfers first, reject successful ancillary and pidfd descriptor delivery,
trace ioctl flag changes, share executable mappings across CLONE_VM, preserve all
compressed bytes when the bounded lifetime observer fails, drain compression after
a root-exit wait timeout, and require tracer-status agreement for complete capture.
Focused controls cover both duplicate-endpoint orders, anonymous mappings across
vfork-like clones, oversized/malformed record retention, ancillary delivery and
injected tracer-drain timeout. The first boundary run failed only because successful
compression added an unnecessary null metadata field; retaining the existing
success schema corrected it. Both logs remain in build/review-boundaries-*.txt.
Final changed-area suite: 226 tests passed in 14.164 seconds
(build/review-boundaries-confirmed.txt). These corrections postdate the real
collector/replay above; the final lifecycle tests exercise actual strace processes.
No fresh whole-phase or real-client qualification is claimed.

GCC's final C/runtime candidate check remains `check.sh finished: pass` in
build/pause-native-final-gcc.txt; later changes are Python and non-embedded docs.
Clang, targeted sanitizers, formatting/dependency checks and pause measurements
remain as recorded in the pause development note. Full-session resource bounds,
actual serving-model evidence, real-client transport qualification and final
four-slot publication remain incomplete. No release/merge approval is implied.

## Real-client development collectors (2026-11-10)

Declared before launch (private `~/.mo-phase5-harness/real-collector-development-20261110/declaration.json`):
exactly one DEVELOPMENT Codex slot and one DEVELOPMENT Claude slot through `harness.py run
--development --limit 3600` at commit c5e6fa19, no replacements, scored 0. Predictions: both
traces parse with no rejected transfer; channels attested; provenance qualified unless a real
session makes an unclassified provider request or bills an auxiliary model.

First attempt: both slots refused **before launch** (`IdentityError`, launched=false, no model
run). The Nov 8 install left `/opt/mo-phase5/bin/mo` at mode 0755; the capture requires no write
bits, and current `harness.setup` already applies `chmod 0555`. That one step was applied
(SHA-256 `517da061…` unchanged), the refused outputs were retained as `prelaunch-refusal-*`,
and the same declaration ran. Cumulative development model attempts are now Codex 4, Claude 2.

| Slot | Client | Wall | Exit | Decoded trace | gzip | Collector peak RSS |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | Codex | 1212 s | 0 | 3,666,228,166 B | 447,596,106 B | 461,740 KiB |
| 2 | Claude | 619 s | 0 | 1,280,413,604 B | — | 254,224 KiB |

Both agents completed the upgrade: task state, activation identity and source tests passed.
**Both captures failed process integrity**, so nothing was consumed and command origin, revision
observation, traffic completeness and model provenance all failed closed. The predictions about
capture were wrong; the failures are retained.

- **Codex:** `ambiguous child ordering`, `unregistered descendant`, `unsupported isolation or memory
  transition`, `unsupported kernel transfer`. Cause: Codex's own `--sandbox workspace-write` runs
  commands through `/usr/bin/bwrap` (114 execs, 76 `unshare(CLONE_NEWUSER…)`). PID namespaces make
  `clone` return namespace-local PIDs, and the cleared descriptor tables make later file copies
  (725 `copy_file_range`) and one `sendfile` from a `/tmp/queue-upg…` file unprovable. Supporting
  this inside the capture is not a narrow repair: a mount namespace can rebind
  `/opt/mo-phase5/bin/mo`, which would defeat path-based push identity.
- **Claude:** only `unsupported kernel transfer`: one `copy_file_range` by the client's own
  multi-threaded process backing up `~/.claude.json` (shared descriptor table, so no regular-file
  credit). Single-threaded `cat`/`cp` copies were credited as designed.

**Provenance route on the real streams (adapters run directly, development only).** Codex:
38 logged `/responses` attempts, 38 provider responses, all `gpt-6.1-sol`/`xhigh`, usage reconciled,
no failed attempts, no unclassified provider request, one thread. Claude: 34 requests, all
`claude-opus-5-5`, input usage equal to terminal `modelUsage`, no other model. These would qualify
once the channels are attested; they are not attested here because the capture failed.

Resource note: at this rate a 3600 s Codex slot is about 11 GB decoded and 1.3 GB compressed
trace. That is a single-session observation, not a bound.

Next work depends on a decision about Codex's sandbox (it is the evaluation condition all earlier
experiments used); see `docs/status/phase5-run.md`.

### Requalification pair (2026-11-10, after the sandbox decision)

Declared before launch (`real-collector-development-20261110-two/declaration.json`, commit
a4ff8621): one Codex and one Claude DEVELOPMENT slot, no replacements, scored 0. Cumulative
development model attempts: Codex 5, Claude 3.

| Slot | Client | Wall | Exit | Decoded trace | Process integrity | Reconciliation |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | Codex (`danger-full-access`) | 1581 s | 0 | 4,991,762,484 B | verified, no errors | `TraceError` |
| 2 | Claude | 410 s | 0 | 875,206,012 B | verified, no errors | `TraceError` |

Both agents again completed the upgrade with correct state and activation identity. The
sandbox decision and the `copy_file_range` rule cleared every process-integrity error, which
was the prediction. The prediction that both traces would parse was wrong. Re-parsing the
sealed traces found four parser defects, each fixed with a test and retained here:

1. A resumed call kept the space strace prints before `<unfinished ...>`, so resumed `pipe2`
   and vector transfers (`writev(…, 2 ) = 543`) failed exact matches. The pending prefix now
   drops that one separator.
2. DNS over UDP (`UDP`/`UDPv6` sockets) hit the TCP-only rule. UDP cannot reach the TCP proxy;
   it now joins NETLINK and UNIX as an explicitly skipped transport.
3. Codex threads make single pipe and stdout writes of about 1.1 MB, above strace's
   `-s 1048576`. Any truncated transfer aborted the whole parse. A truncated pipe, stdout or
   stderr transfer now voids only that event (its pipe or channel then fails closed), and the
   capture uses `-s 4194304`, with the trace record cap raised to 32 MiB to fit `-xx` output.
4. Bun writes Claude's stdout through descriptor 5, a duplicate of fd 1, so the client's own
   output looked unwritten. Channel writes are now matched by the file the descriptor resolves
   to, using the stdout and stderr paths sealed in the capture manifest.

Re-run over the sealed evidence with these fixes: the Claude session's provenance **qualifies**
(stdout attested to the client process, 22 requests, input usage equal to `modelUsage`, only
`claude-opus-5-5`). The Codex ledger is complete (54 attempts, 54 provider responses, stderr
attested), but stdout is not attested: two of Codex's own JSONL writes (1,107,176 and 1,057,620
bytes) were truncated by the old 1 MiB limit. A new declared pair is needed to show both
end to end under the fixed capture.

### Third pair and offline regrade (2026-11-10)

Declared before launch (`real-collector-development-20261110-three/declaration.json`, commit
fa0daefe), one Codex and one Claude DEVELOPMENT slot, scored 0. Cumulative development model
attempts: Codex 6, Claude 4.

Both captures passed integrity and both traces parsed, as predicted. Both slots then failed
reconciliation with `JoinError`, which was not predicted: each MCP client opens a `GET` event
stream, the upstream refuses it, and the base proxy answers with a synthesized 502 whose body
digest the Phase 5 recorder never took (16 of 16 requests matched otherwise). The recorder now
rebuilds a synthesized reply's digest from the recorded text when, and only when, its length
equals the recorded byte count. Run three's proxy logs were written before that fix and stay as
recorded.

To find the next failure without another model run, the protected regrade was run over the two
sealed bundles with the corrected 502 rule applied to the rows in memory (development diagnosis;
the sealed files are unchanged). It found one more defect: observation compared senders by thread
ID, and both clients are multi-threaded (Codex's pipe reads and stdout writes happen on different
tokio threads). Senders now carry a process identity (thread-group ID with the leader's
incarnation), and HTTP receipts carry it too. Results after both fixes:

- **Codex: all seven graded dimensions pass** (task state, activation identity, command origin,
  final answer, source tests, traffic completeness, model provenance). Publication integrity is
  not graded by the regrade.
- **Claude: six pass; final answer fails** with "post-activation revision observation not proven".
  Claude ran its revision `curl` inside a larger Bash command, so the shell output combines the
  revision response with other responses and status lines. The observation rule accepts only a
  shell result that is exactly the response (or the bare value) delivered by one writer, and
  rejects mixed output. The agent did see the response; the instrument cannot attribute it.

### Mixed shell output (Robert's decision, 2026-11-10)

Robert chose to accept the revision response inside larger shell output, with an exact chain
and no new agent instruction. The rule: the process that received the response must have written
exactly that body (same SHA-256 as the traced HTTP response) to its standard output; the client
process must have read it from the same pipe or file afterwards; and the typed shell result the
client emitted must contain it, all after the commit and before the final message. Claude's shell
writes command output to a regular file that the client reads with `pread64`, so the trace now
records writes to a process's standard-output file and later reads of that file.

Controls reject a different writer forging the same bytes, a client that never read the object,
altered bytes, a read that precedes the write, an uncaptured transfer and a pipe whose creation
was not observed. Re-run over the two sealed run-three bundles (corrected 502 rule applied in
memory, sealed files unchanged): **Codex and Claude both pass all seven graded dimensions.**
This is development evidence from one session per client, not the scored cohort.

Code comments added earlier on this branch were removed; their reasoning is in these records.

### Second independent review (2026-11-10)

Six reviewers and a validator reviewed a4c0c3a4..b04a5eb3 (one session model, adversarial lens
in-process, no cross-model peer). Security and correctness traced the `copy_file_range` credit,
channel matching, per-event voiding, process identity and the 502 digests and found each fails
closed. Three findings were confirmed and are fixed:

1. **False pass in mixed delivery (P1).** The rule compared only the client's latest read with
   the receiver's last write and searched all reads together. A second process could write the
   predicted body first, the client could read it, and any later read of the same object then
   satisfied the rule. Now only client reads that finish after the receiver's first write, and
   before the typed result, are searched. The reviewer's probe returned `True` before the fix
   and `False` after. The suggestion to reject any other writer on the object was not adopted:
   it would undo the mixed-output decision, since Claude's shell file always has several writers.
2. The recorded `observation_coverage` text still said mixed writers reject. It now states the rule.
3. The mixed-delivery guards had no negative tests. Controls now cover an early forged read, a
   write before the response arrived, a read after the typed result, a result that omits the
   body, and two matching objects.

Also fixed from residuals: client stdout bytes now round-trip when a write splits a multi-byte
character, and the collector refuses to run below Linux 5.3, the kernel floor the
`copy_file_range` credit depends on. Both sealed run-three bundles still pass all seven regraded
dimensions after these changes.

Open risks, not fixed: Codex remote compaction in a long session may not reconcile with turn
usage and would fail provenance closed; a vectored transfer above the 32 MiB record cap fails
the capture; a client that re-reads its output file only after emitting the result fails
observation; what `codex_core=debug` echoes to stderr is unaudited, and although channel
attestation limits that text to the client's own writes, a forged provider record inside it
is not excluded.

### Pairs four and five under merged main (2026-11-10)

Both pairs were declared before launch, scored 0, no replacements. Cumulative development model
attempts are now Codex 8, Claude 6.

**Operator error (pair four).** The evaluation `mo` was reinstalled from a checkout build whose
embedded library path the session account cannot read; the earlier frozen binary had been built
in a separate readable tree. The first launch was refused before any model ran, for a different
reason: data persisted by the older binary is adopted at startup and prints a line before the
listening line. The fixture was rebuilt with the installed binary (source byte-identical,
`expected.json` equal). The relaunch then ran both clients against a `mo` that could not compile
for them. Claude reported exactly that and stopped. Both sessions are invalid as confirmation
and are retained.

**Capture false pass found by pair four.** Codex worked around the unreadable library by running
`mo push` inside its own `bwrap` mount namespace with a library it reconstructed, and the capture
credited the push. Process integrity flagged `unshare` and `setns` but not namespaces created
through `clone` flags (`clone(child_stack=NULL, flags=CLONE_NEWNS|CLONE_NEWUSER|SIGCHLD)`, eight
calls). Any `CLONE_NEW` flag on `clone` or `clone3` now fails integrity, and that sealed trace now
fails as it should.

**Repairs before pair five.** `mo` is built from e53a822e in `/opt/mo-phase5/build-e53a822e`
(root-owned, tree mode 0711, `lib` world-readable) and installed through `harness.py setup`;
client pins are unchanged. The collector now refuses to launch unless the session account can
compile the supplied source, which would have stopped pair four before it spent two sessions.

**Pair five, live.** Codex passed every evidence dimension end to end with no repair applied
afterwards: capture integrity, command origin, revision observation, traffic completeness and
model provenance (32 of 32 responses). It failed the immutable source tests, and so did its
pair-four candidate. That is the agent's work: both candidates changed `expose Queue,` to
`expose Queue(..),`, which lets the property test build a queue with `next_id` at the largest
integer, where `submit` overflows. The original source and both pair-three candidates pass the
same tests under the same binary.

Claude completed the task correctly but its reconciliation failed with `JoinError`. Each MCP
client opens a `GET` event stream that `mo serve` never answers in that form; the proxy ends it
with a synthesized 502 after its 300-second upstream timeout. Sessions longer than that have the
reply recorded. Claude's 333-second session ended first, so the request had no reply in the proxy
log or the trace and the join rejected the whole session. The join now records a request that is
unanswered on both sides as explicitly unanswered instead of rejecting. Traffic completeness is
unchanged for tool calls: an unanswered tool call still fails, and a reply present on only one
side still rejects. With that change, and no in-memory patching, the protected regrade of the
sealed pair-five sessions gives Claude all seven dimensions and Codex six.

### Review of the pair-four and pair-five repairs (2026-11-10)

A focused review (own read plus one separately dispatched adversarial reviewer on the same model
family; no cross-model peer, no validator) checked the three gate changes. It confirmed that an
unanswered tool call still fails traffic completeness, earns no command origin and no observation
credit, and that a reply on only one side still rejects. It found one defect, now fixed: the
proxy log stayed open through live grading, so a 502 synthesized after the client had exited
could be logged with no trace response and reject a genuine session. The log now closes as soon
as the traced session returns. The counters `verified_push_requests` and
`verified_shell_requests` no longer include unanswered requests.

Open risk, not fixed: strace reads `clone3` flags from user memory, so a deliberately racing
multi-threaded process could hide a namespace flag. Closing it in the kernel means setting
`user.max_user_namespaces=0` for each session, a host-wide change that would also break other
sandboxed tools on the machine for that hour. That needs the owner's decision. A short window
also remains between the client's exit and the log closing.

The no-model collector test (`test_collector`: success, curl, urllib, timeout and cleanup-failure
cases with a stub client under a separate test account) passed in 563.047 seconds with the
launch-path changes, using the evaluation binary.
