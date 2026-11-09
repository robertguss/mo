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
