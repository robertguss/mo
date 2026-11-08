# Phase 4 run

**Diagnostic gate passed on 2026-11-07: 14 of 16 correct. Robert approved v0.1.0
and the squash merge of PR #308 on 2026-11-07; Phase 4 is complete.**

Codex was correct in 8 of 8 sessions, Claude Code in 6 of 8. Scenarios A, B and C each
passed 4 of 4; scenario D passed 2 of 4. Both misses were Claude Code fixes that failed
hidden tests despite receiving `verified` from the separate fix checker. The original
reproduced in both, so neither was an original-first violation. All record matches passed.

The [full results](../../learnability/phase4/evaluation/results.md) report every slot,
coverage, fix checks, misses, predictions against results and publication scans. The
[protocol](../../learnability/phase4/diagnostic-protocol.md) and
[freeze](../../learnability/phase4/freeze.json) define the experiment. This is a capability
result for four scenarios, not comparative evidence or a population success-rate claim.

## Environment and authorization

| Setting | Recorded value |
| --- | --- |
| Build | `40ca1848b4cdfcc0513dc3bfc2fa67432bc9cf35`, GCC 13.3.0 debug, `mo 0.1.0` |
| Candidate | `/opt/mo-phase4/candidate/build/gcc/debug/mo`, installed as `/opt/mo-phase4/bin/mo` |
| Candidate SHA-256 | `7d4dae15db2aa9aa6dabb6134ca56e87abd5658eae00aba44fdb6624564f4ba6` |
| Clients | Claude Code 2.1.317; Codex 0.166.0; complete digests and command lines in the freeze |
| Models | `claude-opus-5-5`; `gpt-6.1-sol`, reasoning effort `xhigh` |
| Session account | `mo-session`, isolated working folder and read-only starting data/deployed source |
| Allocation | Four scenarios, twice per client, 16 sequential slots; seed 20261107 |
| Limits | 3,600 seconds per session; Robert authorized no monetary cap |
| Session window, UTC | 2026-11-07T19:33:48.787763+00:00 to 2026-11-07T20:56:58.540493+00:00 |
| Authorization | Robert answered “Yes” on 2026-11-07 to collection against freeze commit `d4df49ef`; recorded separately so the frozen protocol hash stayed unchanged |

Before collection, all 269 candidate build-input files matched the pinned commit. Installed
binaries, script and protocol hashes, the four private scenario exports, round metadata and
order matched the freeze. All four fixtures built on the first attempt; the timing fixture
took about 32 minutes. `grade.py --check` reported `phase 4 variants: 4 of 4 sound`, including
recomputed question keys. Pilot variants were excluded from the exported scenario root.

## What the record supports

Correct sessions on both clients used operator tools, fetch, history, query and replay.
Only Codex demonstrated `mo replay --fix` in a correct scored session. Coverage is conservative:
a command hidden inside a session-written script can be missed. The grader’s fix check is
separate from session tool use and is never the correctness oracle.

Every declared slot ran once. There were no missing sessions, unstarted/discarded attempts,
timeouts or model substitutions. Slots 15 and 16 took 986.0 and 1045.6 seconds and failed a
hidden test. Their HTTP connection-reset exceptions remain in the round log; both record
matches passed. No failed session was rerun or dropped.

The overall prediction of 14 correct held, but the predicted distribution did not: A exceeded
its prediction, D fell below it, and Claude Code fell below the predicted 7 of 8. More sessions
used fetch and query together on B than predicted. The original predictions remain intact.

## Publication

[Evidence release](https://github.com/robertguss/mo/releases/tag/evidence-phase4-diagnostic-2026-11-07) includes all client streams and proxy logs, original grades and
manifests, service/client stderr, round output and the committed private scenario history.
Both archive scans passed, before packing and after extraction; the extracted files were
also compared byte-for-byte with the stage. The history was restored in a new bare repository,
and its original commit and every frozen scenario hash matched. Uncommitted pilot edits and
private notes remain untouched and unpublished. [Collection notes](../../learnability/phase4/evaluation/notes.md)
record authorization and the absence of reruns.

## Verification inventory and limits

The collection checks below ran on the frozen candidate. The implementation evidence is
historical evidence from the linked milestones, not a claim that all proof and test groups
were rerun on this candidate.

| Check | Recorded result | Boundary |
| --- | --- | --- |
| Final branch GCC suite | `bash ci/check.sh gcc`: `check.sh finished: pass`; no skipped checks reported | C build and unit suite; other groups not rerun for these nonembedded document/result changes |
| Publication and record integrity | Both archive scans clean; downloaded archive digest matches; all 144 manifest artifact hashes and an independent grade tally match; 49 local document links resolve | Credential scan covers the recorded tokens and encodings; hashes establish consistency, not application correctness |
| Candidate build and freeze comparison | GCC build succeeded; 269 build-input files matched the source commit, root-owned and read-only; installed binaries, scripts, protocol, scenario files and round/order matched before collection | Build identity and input consistency, not whole-runtime correctness |
| Four scenario soundness checks | `phase 4 variants: 4 of 4 sound`; question keys recomputed from built data; reference fixes pass and unfixed/broken cases fail | These four scenarios and their fixed tests; not exhaustive application correctness |
| Native-client smoke | Both pinned clients completed the six named instrument steps; records matched | Development evidence with instruments named in the prompt; excluded from scoring |
| Lean saved-schema proofs | `decode_encode`, `decode_checks_schema`, `decode_refuses_other_schema`, `decodeInputs_encodeInputs` passed in the offline milestone | Fitting modeled values and schema-byte equality; recursive schemas, injectivity of schema encoding and byte-for-byte C/Lean encoder equivalence are not proved |
| Lean verdict proofs | `signaturesMatch_iff`, `verdict_verified_iff`, `verdict_sound`, `verdict_not_verified_of_not_reproduced`, `verdict_table` passed; negative controls and axiom guards passed | Abstract signature/verdict rules; no proof that all C execution refines Lean. Only `propext`, `Classical.choice`, `Quot.sound` allowed; a planted `sorry` was rejected |
| VM-versus-Lean offline cases | Three seeded handlers agreed on `stop`, integer overflow and division-by-zero outcomes in GCC and Clang | Work, heap/stack/depth and saved-size limits excluded; source locations and arrival-time behavior not compared |
| CBMC reader and framing proofs | `export_page_matches`, `slice`, `mo_failure_argument_next`, `mo_log_record_walk`, `mo_export_page_bounds`, `mo_export_schema`, `mo_export_read` passed with reach checks in the offline milestone | Bounded C harnesses and stated modeling assumptions, not SQLite, transport, SHA correctness or a whole-runtime proof |
| TLC export model | Finite safety/progress instances and two negative controls passed in the export work | Collision-free IDs abstract entropy; progress assumes weak fairness and disables replacement/expiry/restart; no C or transport proof |
| TLC capture model | Strengthened finite instance passed and each of six negative controls found its named counterexample | Three kinds, four failures, three retained records, two updates; byte cap unmodeled and SQLite atomicity assumed; no power-loss proof |
| Live-view tests and fuzzing | Compiler/sanitizer suites, HTTP behavior tests and final renderer fuzz smoke passed in that milestone | Finite empirical evidence; an earlier combined timing failure is retained, with an isolated passing rerun |
| Fetch checks | GCC, Clang, sanitizer, all-build drivers, static/models, scoped CBMC/reach, native smoke and maximum-size oracle passed in the fetch milestone | One host and fixture; deadline assumes scheduling and interruptible POSIX calls. SIGKILL, machine failure and filesystem failure can prevent cleanup |
| Failure-capture checks | Compiler/sanitizer suites, models, scoped CBMC/reach, fuzz and 100-kill recovery run recorded passes | Initial driver group failed its timing gate; a subsequent startup fix passed timing. Earlier failures remain in the evidence |
| Offline acceptance, kill loop and grader cross-check | Acceptance passed; 60-kill run passed; unchanged/reference/broken-fix matrix behaved as declared | A broken fix can receive `verified` and fail the independent grader. Queued-mailbox memory non-reproduction is unit evidence, not a command-level live-service demonstration |
| Other check groups on this collection candidate | Clang, sanitizer, drivers, static, Lean, models, CBMC, fuzz and bench were not rerun during collection | Not-run groups are not fresh passes. Earlier milestone results retain their own build identities |

Sources: [live views](../../learnability/phase4/live-views/README.md),
[fetch verification](../../learnability/phase4/private-fetch/results/verification.md),
[failure capture](../../learnability/phase4/failure-capture/README.md),
[offline verification](../../learnability/phase4/offline/results/verification.md),
[model assumptions](../../models/README.md), and [native smoke](../../learnability/phase4/smoke/README.md).
The offline milestone's combined run retained a `bench` failure from a stale proxy catalog
expectation; the evaluation harness work subsequently recorded passing `bench` checks in PR #308.

## Release decision

Robert answered “yes I approve” in this continuation conversation to the explicit
request to release v0.1.0 and squash-merge PR #308. Annotated tag `v0.1.0` points at
squash-merge commit `fad1467d`, after comparing every build-input Git object on that
commit with `freeze.json`. The candidate reports `mo 0.1.0`; the two evaluation misses
remain part of the published record. This approval changes no frozen experiment input.
