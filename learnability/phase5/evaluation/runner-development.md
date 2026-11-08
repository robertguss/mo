# Runner integration development

FINAL remains disabled. No scored sessions were launched, and release requires
Robert. The integration has seven passing focused runner tests; this is not an
OS isolation or end-to-end collector attestation.

Private SPEC requires absolute source, context, data, prompt, expected and
experiment paths. expected contains private jobs and revision "1". experiment
is a protected shared directory for the entire ordered four-slot experiment.
Slots are reserved under flock immediately before process launch, using exclusive
creation; an existing reservation cannot be replaced. A preceding slot must have
a final manifest before the next reservation. Prelaunch fixture refusals do not
reserve a slot and have launched=false. Every attempted launch retains its slot.

The runner appends the migration-name requirement `migrate` and the final-answer
rule to the saved prompt: last assistant message, stripped, exactly `1`.
It compares private jobs before launch, observes live jobs/revision before stop,
collects edited source after killing the session processes, restores queue tests
in a separate grading copy, and derives both MOSB identities with `mo check
--upgrade-from` against the retained original source. No reference candidate
digest is used. It invokes stopped_grade after stopping the original process.
The manifest contains slot/client/correct/development for evidence.py, plus the
hidden verdict, identity, failures, launch status and artifacts. Missing model
evidence, substituted models, reported fallback, unsuccessful exit, timeout or
incomplete grading are misses. DEVELOPMENT records cannot satisfy the final gate.

Legacy imports are isolated during module loading and restored afterward, keeping
Phase4 fixture references separate from Phase5 grader references. Both credentials
remain environment references, including serve --upgrade-token-env. The existing
separate Phase5 account, private paths, neutral restart and iptables denial remain.

Freeze now checks origin/main and accepts hashed independently checked local repair
verification: verification_evidence names JSON with commit, independently_checked
and passed; the latter two must be true. The verification artifact group replaces
nightly. This is a provenance input contract, not independent authentication of
those assertions. Clean candidate, ancestry, binary and artifact checks remain.

Outstanding required proof: unauthenticated distinct-user OS stub setup and full
collector exercise; real proxy push with DEVELOPMENT stream and wrong/no command
and failed commit controls; independently authenticated control/freeze evidence.
Passwordless sudo and `sudo -n iptables -L OUTPUT -n` succeeded here; no OS blocker
was established. FINAL must stay disabled until these are completed. Existing
controls-development.md documents the eight service controls separately.

Validation: `python3 -m unittest discover -s learnability/phase5/evaluation -p
test_harness.py -v` — seven tests passed. No C builds, measurement edits, commits,
PRs or agent sessions were performed.

## OS stub attempt (2026-11-08)

`python3 -m unittest discover -s learnability/phase5/evaluation -p test_collector.py -v`
uses only `mo-phase5-test`, temporary installs/private state, explicit fake login
files and a Phase4 home snapshot. No provider authentication is used. The stub
checks both credential environment variables, private-folder denial and direct
backend denial, edits the candidate, and attempts real `mo push` with environment
references. A separate timeout case spawns a child and captures a partial edit.
The reference installed for this test is DEVELOPMENT-only.

Failures retained: initial test setup created root-owned credential directories
(`tee: Permission denied`), fixed with owned directory creation. Collector service
readiness blocked because its buffered pipe disagreed with Phase4's select/read
loop; fixed with `bufsize=0`. That run was terminated with SIGTERM and cleaned up.
The subsequent real session reached the stub but `mo check main.mo --upgrade-from
INSTALL/session/deployed/main.mo --migrate migrate --json` returned E1419,
`the library folder can't be inspected`. The supplied binary embeds
`/home/exedev/code/mo-m6/lib`, inaccessible to this session UID. No production
build or permissions outside the assigned checkout were changed. Source grading
also returned E0050, `no visible function has this dot-call name`, at the migration
in main.mo after supplying a test-only UpgradeOld module. Thus this is a failed
collector proof, not successful push/live/restart attestation or a real score.

The seven existing `test_harness.py` tests pass after the pipe fix. FINAL remains
disabled. Full command output for this execution is in
`/tmp/phase5-collector-results.txt` and `/tmp/phase5-harness-results.txt`.

Completed result: 1 test, 113.102 seconds, one failed success subcase. The timeout
subcase passed: partial source captured, timeout recorded, private folder unreadable,
backend connection denied, both credentials present, no session processes left,
and iptables OUTPUT identical to its pre-run baseline. All test processes finished.

## Readable build and historical source adaptation (2026-11-08)

The bounded test now requires MO_EVALUATION_BINARY explicitly. Built with
`make -j8 CC=gcc` in `/tmp/mo-eval-build-aanyuygb`, copied from this checkout:
only src, lib, vendor, runtime (required by embed_protocol.py), Makefile,
ci/embed_docs.py, ci/embed_protocol.py, docs/reference/README.md and
docs/agent-guide.md. No evaluation, tests, credentials or hidden fixtures were
copied. Root mode is 0711; src/vendor/runtime/ci/docs are 0700; lib retains
read/traverse permissions. No home or existing checkout permissions changed.
Build log: `/tmp/mo-eval-build.log`. The embedded absolute library path is
readable by the session; this is a staged installation, not a path-independent
binary. Parent must retain/freeze this root and binary before release.

Queue(..) eliminated E0050. The first readable-build run reached real proxy push,
live revision 1 and successful stopped/restart grading, but source tests stopped
on UpgradeOld.submit's overflow promise: exposing historical Queue internals made
an impossible internal next_id available to promise generation. Full failure log:
`/tmp/phase5-collector-readable-results.txt` (186.278 seconds, one failure).

Private source grading now replaces candidate UpgradeOld with type declarations
from operator-retained serve/job-queue.mo. The synthetic upgrade namespace needs
historical types, not old executable functions/promises. All immutable queue tests
and candidate tests still execute; typed --upgrade-from independently verifies
the migration against original source. Unsupported historical declaration shape
fails closed. Test expectations remain operator-owned.

Input/binary SHA-256 inventory: `/tmp/mo-eval-build-inputs.json` (286 files).
Successful run artifacts retained privately in `/tmp/phase5-collector-proof`
(mode 0700): manifest, immutable source-test output, stream, proxy and restart log.
The standalone historical-type adaptation produced 47 passes, zero failures and
zero stopped tests. Seven test_harness.py cases also pass after the adaptation.

Completed full rerun:
`MO_EVALUATION_BINARY=/tmp/mo-eval-build-aanyuygb/build/gcc/debug/mo python3 -m unittest discover -s learnability/phase5/evaluation -p test_collector.py -v`
passed both success and timeout subcases. Complete output:
`/tmp/phase5-collector-final-results.txt`. Assertions cover credential environment
references and stream redaction, real proxy push provenance, live/restart grading,
immutable source grading, partial timeout capture, private directory and backend
denial, no session processes and exact restoration of iptables OUTPUT rules.
No scored sessions, real provider authentication, commits, PRs, production runtime
changes or gate changes were made. FINAL remains disabled; freeze/release and
independent review belong to the parent. All launched checks have completed.

## Integrated collector after persistent-state controls

The full OS success/timeout exercise passed again in 204.045 seconds after the
fixture preparation history, persistent revision and next_id checks were added.
Command: `MO_EVALUATION_BINARY=/tmp/mo-eval-build-aanyuygb/build/gcc/debug/mo python3 -m unittest discover -s learnability/phase5/evaluation -p test_collector.py -v`.
Raw output: `build/m6-collector-integrated.txt`. FINAL now requires the immutable
freeze and a hashed control receipt; no scored session has run.
