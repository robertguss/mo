# Live upgrade evidence — phase open

The 1,000-kill drill passed. The production pause series missed its declared
2× margin in the debug build. The declared agent evaluation missed its gate.
This is v0.2 preparation, not a release: no tag, deployment or phase closure is
claimed. Robert's explicit release approval remains required.

The repaired main commit is `814ddd0f70e5fd613130fe87a9cd24cd187af8e6`
(PR #317). It was merged and independently verified before final collection;
no unmerged nightly repair was used. Local verification is recorded below;
this is not a claim that the remote nightly was green.

## Frozen inputs

The original collection candidate was `3a41f17b2129fb19f2fb5c9ce89321087dac7173`,
with freeze SHA-256 `3831de803b57cef3229ecdc2aca698d4c21a87e46383de3ffb8c85faa6908ae7`.
Experiment 2 used `10499452286bda1b85e8e43775175bd1a69169b4`, freeze
`c49ecdc32334847a0dc63fa8b2b211edfea917d4e1d56a42a0b2a6cb7e654995`.
All 285 build inputs, binaries and drill/pause instruments matched between
freezes. The latter changed client credential setup and publication handling.
The installed evaluation binary was built in its retained library tree; its
source bytes match the checkout. Build commands, compiler versions, binary
hashes, repair verification and comparison are in the [evidence directory](../../learnability/phase5/results/).
The comparison against repaired main changes only the two instrumented C files
and their compile-time timing header. No release tag is available to compare.

## Drill and recovery

[Declaration](../../learnability/phase5/collection.md): 1,000 process kills,
seed 20261110. The frozen run passed in 2,865 seconds: 25 folders, 1,000 history
reads, 53 mid-commit recoveries, 15,163 calls, 14,447 answered, 152 refused and
716 without replies. These counters have the existing reply model's meanings;
refusals are not additional accepted calls. Every restart reconciled with an
allowed reply-model outcome; no accepted message was lost or duplicated.

There were 235 pushes, 75 activations and 88 invalid attempts. Refusals were
E1426 ×85, E1423 ×1, E1417 ×1 and E1416 ×1. The schedule covered tool-only
changes, a field migration, broken migration, oversized source and stale base.
Of 116 requested push-kill attempts, 79 killed an in-flight push; the others
finished first. The actual 1,000 kills comprise 920 ordinary-call kills, 79
push kills and one post-migration kill. Folder-only restart checked program and
activation ordinal. The randomized run does not claim internal-stage visibility.

Six separately declared debugger controls on the frozen binary observed upload,
compiled staging, an ordinary-request parsing VM instruction, migration execution, transaction
interior and postcommit/pre-reply. All passed job/deadline preservation and
activation/retry checks. Their raw instrument retains its DEVELOPMENT label;
these controls do not enlarge the random-kill or agent-session denominator.
Process kills are not power-loss tests. Earlier missed push kills and the
retracted stale-binary breakpoint run remain in the [development record](../../learnability/phase5/drill-development.md).

## Production pause

Five debug and five optimized repetitions used 5,000 distinct 2,048-byte jobs
and 500 leases. All ten identity transfers committed ordinal 1 and passed the
fixture's exact state verifier before and after activation, including next_id,
payloads, attempts, states and lease deadlines. No queued caller arguments were
included; the earlier prototype's 64-argument stress case remains separate.

| Build | Min | Median | Max | Sample SD | Declared max ≤0.5 s |
| --- | ---: | ---: | ---: | ---: | --- |
| GCC debug | 0.449489 s | 0.485799 s | 0.554238 s | 0.039447 s | Miss |
| GCC optimized | 0.248552 s | 0.273589 s | 0.284485 s | 0.014547 s | Pass |

Every observed pause was below the one-second engineering target, but the debug
series missed the declared twofold margin. Each report charged 5 VM and 619,287
native work units, 11,967,960 bytes heap peak and 10,464,325 encoded bytes. Work
budgets are not wall-time guarantees. This measures unchanged-schema identity
transfer; the earlier explicit full-size map reconstruction was refused E1424.
Small field-migration checks do not establish full-size reconstruction latency.

The host was an exe.dev x86_64 VM on AMD EPYC 9554P, Linux 6.12.113, roughly
32 GB RAM, guest `/dev/vda` ext4. Physical storage is not exposed. Production
SQLite used DELETE journaling, EXTRA synchronization and MEMORY temporary
storage. Compile-time monotonic probes emit synchronously to stderr, adding
measurement overhead. Raw stages distinguish cached old-state access from
encoding: identity decode, output/retained-byte encoding, validation decode,
timer validation, admission, catalog, swap preparation, activation report,
store activation and boundary-to-commit. Nested stage totals must not be summed.
Compiler/load controls separately accepted 64 generic instantiations and 255
parenthesis nesting levels, refusing 256 with E1419; this is a specific syntax
family, not a universal compiler maximum.

All samples and stage reports are in [results](../../learnability/phase5/results/).
Earlier timeouts, interrupted readback, failures and corrected observations remain
in the [measurement log](../../learnability/phase5/measurement-development.md).

## Evaluation

The [protocol](../../learnability/phase5/protocol.md) declared four slots in
Codex/Claude/Codex/Claude order, a 3-of-4 gate with at least one pass per client,
frozen client versions/models, 3,600-second limits and hidden state/restart
checks. Development runs never enter this denominator. Actual model metadata,
failed attribution, final-answer misses and all original grades are published.

Experiment 1 launched one session and was interrupted when the collector's
native MCP upgrade header was found to omit `Bearer `. Three slots were not
launched; their absence is recorded, without fabricated streams. This was an
instrument failure. A separately declared experiment 2 corrected the header
before its four sessions. No session was replaced within either experiment.

Experiment 2 scored **0/4**, below the fixed 3/4 gate; both client subtotals are 0/2.

| Slot | Client | Reported model | Original outcome |
| --- | --- | --- | --- |
| 1 | Codex 0.166.0 | Not present in stream | Miss: model metadata absent; qualified-name grading failed; proxy calls unmatched |
| 2 | Claude Code 2.1.317 | claude-opus-5-5 | Miss: unmatched calls and final answer not exactly `1` |
| 3 | Codex 0.166.0 | Not present in stream | Miss: model metadata absent; qualified-name grading failed |
| 4 | Claude Code 2.1.317 | claude-opus-5-5 | Miss: unmatched calls and final answer not exactly `1` |

Client durations were 308.341, 109.801, 303.292 and 168.547 seconds. None timed out.
Both Claude sessions retained the requested model; this does not erase the
historical preflight fallback. Codex was requested as gpt-6.1-sol at xhigh, but
missing actual stream model metadata remains a miss. Original records are in
[experiment 1](../../learnability/phase5/results/experiment-1/) and
[experiment 2](../../learnability/phase5/results/experiment-2-original/).

The original slot-1 grader also assumed `Main.migrate`, although the protocol
allows a function named `migrate` in another module. A separately named
post-collection analysis uses the accepted report's qualified selector and
independently recompiles candidate/base identity. It does not waive failed
proxy reconciliation or absent model metadata, and retains original manifests.
Remaining sessions used the original freeze without a mid-cohort instrument
change. The [corrected analysis](../../learnability/phase5/results/qualified-analysis/)
also scores 0/4. All four diagnostic state/restart comparisons pass, while the
original model, proxy reconciliation and exact-answer failures remain. Shell view calls without native stream twins remain explicit unmatched
calls, not silent exemptions. The exact final-answer rule remains unchanged.

## Verification and limits

`bash ci/check.sh gcc`: `check.sh finished: pass`. GCC, Clang and sanitizer suites
passed. The initial combined run ended in failure after the parent edited
`ci/check.sh` while Bash was reading it; its changed file offset caused a syntax
error after driver work. The failed log is retained. The drivers/static rerun
passed without missing-tool skips, and final declared-count drivers finished
`check.sh finished: pass`. The final GCC/drivers/static rerun covers the published source snapshots as well;
it finished `check.sh finished: pass`, with no missing-tool skips. The complete
log is retained in results.

Seeded upgrade_pages fuzzing passed 62,144 executions in six seconds (20261110);
protocol_server passed 12 in 36 seconds (20261111). Each requested five seconds;
the last iteration can exceed that duration. Initial wrong-corpus-path attempts
remain. Timing-enabled ASan/UBSan activation passed a ten-job/one-lease control.
Both compilers built timing-on and timing-off binaries; ordinary binaries omit
the probe format. No Lean/CBMC/model contracts changed, so those groups were not
rerun for M6; this is not a pass claim for skipped work.

[Independent review](../../learnability/phase5/review.md) found seven defects,
which were fixed and checked, plus the retained startup/header/publication
follow-ups. Qualified-name diagnostic correction received separate independent
validation. Publication scans cover both service credentials and provider
secrets, including encoded forms, before and after archive extraction.

Retained limits: the compiler child shares the host identity rather than an OS
sandbox; native accounting does not bound allocator internals, hash collisions,
scheduling or disk latency; initialization can stall; kills can leave scratch
files; shared rate limits permit sustained starvation; parsing remains duplicated;
E1428 deterministic reachability is not established. No active-bundle read tool
was added. Rehearsal needs declared original source/context and omits ordinal;
actual activation reports its committed ordinal. No TLS client or relaxed
loopback/credential ordering was introduced. Synthetic retry refusal is not real
server bucket exhaustion. Earlier Claude automatic fallback remains in preflight;
actual models from this collection are recorded separately. Phase 5 stays open.

## Evaluation capture follow-up (2026-11-09)

The follow-up preserves the original 0/4 result. No new scored sessions have
run. Three declared model-serving development sessions completed: two Codex and
one Claude, with no replacements. They did not qualify actual-model coverage.
The additional Codex HTTP characterization captured 63 SSE events but no actual-model
headers; successful client completion does not satisfy the provenance gate.
Collection remains disabled pending trusted actual-model evidence for both
frozen clients and complete real-client request attribution.

The current repairs reject command-text-only upload attribution, distinguish
state and activation identity from missing attestation, and clarify the entire
final-message rule. Protected stub controls verify correct state and identity
while missing provenance remains a failure. Publication distinguishes protected
independent regrading from public score aggregation; its new regrader interface
is not itself an implemented independent regrader.

Capability inspection and retained failures are in
[model provenance](../../learnability/phase5/evaluation/model-provenance-development.md),
[origin observation](../../learnability/phase5/evaluation/origin-observation-development.md),
[grading](../../learnability/phase5/evaluation/grading-development.md), and
[integration](../../learnability/phase5/evaluation/integration-development.md).
These repairs do not complete the follow-up plan or satisfy the release gate.

Follow-up GCC verification completed with `check.sh finished: pass`; the log is
`build/evaluation-repair-gcc.txt`. The initial integrated Python run completed
25 of 26 tests and errored on the collector's missing binary environment setting.
The corrected collector invocation and publication controls subsequently passed,
as recorded in the integration note. No C or embedded guide/reference/library-map
files changed, so additional compiler, sanitizer, fuzz and drivers/static groups
have not been rerun for this Python-only follow-up.

An independent six-lens review found no remaining defects in the intermediate
capture changes. That verdict does not qualify the full plan, real clients or
publication. Nine live transfer controls passed, including successful direct
positioned I/O on a proven regular file. Protected publication regrading and
actual-model coverage remain incomplete; development stub results cannot replace
those requirements. Detailed failures, corrections and verification are retained
in [transfer classification](../../learnability/phase5/evaluation/transfer-classification-development.md).

The complete protected collector rerun passed all five development cases
(native, curl, urllib, timeout and injected cleanup-reporting failure): two
tests in 462.387 seconds, zero skips. Cleanup and credential restoration were
verified; 81 publication-eligible files passed credential/provider scanning.
This closes the intermediate review's rerun gap. Model and publication dimensions
remain false; no fresh scored cohort has launched.

Protected replay now reproduces success and no-upgrade diagnostics from fresh
sealed development inputs, including retained immutable source tests. The
no-upgrade case returns a complete miss instead of aborting publication. The
original publication downgrade, current-checkout test dependency and missing-report
exceptions have focused regression fixes. A subsequent independent re-review
did not complete and is not an approval. Full updated collector checks and
compilation-failure replay remain pending.

Capture resource qualification is also unresolved: the latest success trace was
145,534,029 bytes for roughly three seconds. The implementation reads and parses
whole traces; its 256 GiB retention ceiling does not establish feasibility for
the declared one-hour sessions. No new scored collection is authorized by these
development results.

The full collector suite after streaming and partial-receipt integration passed
all five declared development cases: three unittest methods in 658.500 seconds
(build/stream-full-collector.txt). Cleanup and credential restoration passed;
82 publication-eligible files passed scanning. The required GCC check completed
with `check.sh finished: pass` (build/stream-final-gcc.txt).

A bounded production replay measurement established that the repeated 598192 KiB
ru_maxrss value was retained accounting across exec: the measured replay kernel
VmHWM was 40880 KiB. This resolves that measurement ambiguity, not full-hour
resource qualification. Remaining growing buffers and model-evidence limits
remain documented in the resource and provenance notes.

Follow-up lifecycle verification (2026-11-09): integrated review found four
failure-path defects in cleanup/source preservation, service quiescence, bundle
fallback and tracer shutdown. Corrections pass 149 focused tests. Exactly two
additional DEVELOPMENT collector controls (success and timeout, no retries or
model calls) passed their assertions; the timeout's protected partial replay
completed in 72.324s with source tests true and the other six grading dimensions
false, preserving all retained pins. Cleanup and credential scans passed. The
success control passed the six non-model dimensions. Independent re-review of
these corrections is pending. Full-duration resource and actual-model proof
remain unresolved; scored collection stays disabled and official 0/4 is unchanged.

The lifecycle re-review's two remaining retention findings are corrected: existing
work blocks reset even if the hold cannot be written, and optional artifact
retention failures no longer abort a partial manifest. Final focused checks:
154 passed. One declared DEVELOPMENT fallback collector and actual callback
replay retained useful state, identity and source diagnostics while denying
origin, traffic, final-answer and model credit. An overly strict identity
assertion failed after replay; the failure and separate saved-verdict validation
are retained, with no repeat collection. Cleanup and credential scans passed.
Model proof, full-duration capacity and phase closure remain unresolved.

## Pause optimization follow-up (development)

Robert authorized runtime optimization while preserving the declared workload,
0.5-second maximum and failed samples. Catalog preparation now counts exact JSON
bytes instead of materializing each complete tool rendering; long strings count
escape occurrences through existing native text operations. Catalog contents,
pagination, validation, storage synchronization and activation boundaries are
unchanged. Nested compile-time probes attribute descriptor, preparation and
retention costs; their synchronous writes remain inside the measured boundary.

The first five-debug/five-optimized confirmation missed the debug target again:
maximum 0.669135 seconds. It is retained in full. After the long-string change,
a separately declared second series observed debug maximum 0.392591 seconds
and optimized maximum 0.222705 seconds. All ten second-series samples used the
original 5,000 distinct 2,048-byte jobs and 500 leases, passed exact state checks
before/after, and committed ordinal 1. These development results meet the unchanged
margin; they do not erase the original miss, establish a wall-time guarantee, or
constitute a release freeze. The [complete follow-up evidence](../../learnability/phase5/results/pause-optimization/README.md)
retains both confirmation series and the locating samples.

Phase 5 remains open. Actual-model provenance, transparent real-client capture,
full-session resource/publication qualification and the fresh fixed four-slot
evaluation remain incomplete; no scored session or release was launched here.

A subsequent independent capture review found compatibility and retention defects.
Follow-up controls cover thread/socket handoff, vector buffers, JIT mappings,
root-exit cleanup, state retention and protected collection binding. The focused
regression suite passed 237 tests; this is not real-client qualification. A Node
keepalive/spawn control exposed successful io_uring transfers that remain
unsupported and are rejected. See the [capture development record](../../learnability/phase5/evaluation/integration-development.md)
for failed controls, repairs and remaining coverage.

## Model provenance route (development, 2026-11-10)

Both frozen clients now have a typed actual-model route, qualified against retained
real captures, with no new model sessions (cumulative development attempts stay
Codex 3, Claude 1; scored 0). Codex: the provider's own `response.model` and
`reasoning.effort` in every traced HTTP response, bound one-to-one to the client's
logged `/responses` attempts and reconciled with its turn usage. Claude: provider
`message.model` per request ID, with per-request input usage summing exactly to the
terminal `modelUsage`, as it does in both original scored Claude streams. A session
qualifies only if the client process itself wrote those channels, checked from the
syscall trace. Codex collection therefore uses the HTTP Responses transport with
diagnostic logging; binary, model and effort pins are unchanged. Details, source
references and limits are in the [model provenance](../../learnability/phase5/evaluation/model-provenance-development.md)
record.

The route is not yet exercised through the full traced harness with a real client.
That, full-session resource bounds and the fixed four-slot cohort remain outstanding;
Phase 5 remains open.

## Real-client development collectors (2026-11-10)

One declared development slot per client ran through the full traced harness (scored 0). Both
agents completed the upgrade with correct state and activation identity, and the provenance
adapters qualified both real streams (Codex 38/38 responses, Claude 34 reconciled requests). But
**both captures failed process integrity**, so the slots failed closed. Codex's own bubblewrap
sandbox uses user and PID namespaces the capture cannot follow. Claude backs up its config with
one `copy_file_range` that the descriptor rule cannot credit. Collection stays blocked on real-client
capture qualification. Details and retained failures are in the
[capture development record](../../learnability/phase5/evaluation/integration-development.md).

Machine-local note: between 01:50 and 02:15 UTC on 2026-11-10, Robert's disk cleanup removed
both checkouts' `build/` directories (and some `/tmp` review trees), freeing about 127 GB.
Build logs that earlier records cite under `build/` (for example `build/pause-native-final-gcc.txt`
and `build/review-*.txt`) no longer exist. The repository records keep their stated results; the
logs themselves cannot be re-read. Private evidence under `~/.mo-phase5-harness` and the
`phase5-test-private-*` roots is intact.

**Decision (Robert, 2026-11-10):** Phase 5 runs Codex with `--sandbox danger-full-access`
instead of its bubblewrap `workspace-write` sandbox, relying on the collector's isolation
(separate account, backend firewall, private folders). This changes the condition earlier
experiments used and is disclosed as such. The descriptor rule also now credits a successful
`copy_file_range` between two non-virtual paths without table facts: since Linux 5.3 that call
succeeds only between regular files, so it cannot carry socket or pipe bytes. Re-run over the
retained Claude trace, process integrity now passes; the run records the host kernel.
