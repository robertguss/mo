# Live upgrade evidence — pending collection

The phase remains open. Preparation began at
`d31c9301132ac9e55432bf9e36855c2914ecab21`. The nightly repair merged in PR #317 as
`814ddd0f70e5fd613130fe87a9cd24cd187af8e6` and is incorporated in this checkout.
Final collection still requires independent verification and a complete input
freeze. No unmerged repair was used.
No release, tag, deployment or phase closure is authorized here.

[Collection declaration](../../learnability/phase5/collection.md) and
[declared evaluation protocol](../../learnability/phase5/protocol.md) separate the
fixed final denominator from development evidence. Evaluation collection is authorized by the implementation request; it requires
a complete artifact freeze. Robert's explicit approval is required for release.

Development evidence is recorded separately in the [drill log](../../learnability/phase5/drill-development.md),
[measurement log](../../learnability/phase5/measurement-development.md),
[grader controls](../../learnability/phase5/evaluation/controls-development.md) and
[collector log](../../learnability/phase5/evaluation/runner-development.md).
Migration/refusal drill variants, production timing probes, real-service hidden
controls and the separate-account collector are implemented. The collector's
successful upgrade and timeout exercises pass, including source tests, live and
folder-only restart grading, credential forwarding, isolation and cleanup.
These are development controls, not scored agent sessions.

The full-size reconstruction migration refused E1424. A subsequent unchanged-schema
identity transfer committed with a 0.540073616-second instrumented debug pause,
missing the declared 0.5-second margin. Its live readback was interrupted; separate
recovery verified the persisted jobs. These observations remain development
results and do not replace the declared debug/optimized repetitions.

Remaining: final internal-stage controls, completed cleanup-failure control,
frozen build/input comparison, final drill/pause collection,
four scored sessions and publication scans. No final gate or release is claimed.

Retained limits: compiler child runs as the host identity, not an OS sandbox;
native accounting is not a wall-clock bound; initialization stalls and scratch
cleanup remain operational concerns; shared rate limits permit starvation;
parsing duplication remains; E1428 deterministic reachability is not established.
Keep all earlier preflight failures and residuals. Rehearsal has no activation
ordinal. Synthetic routing refusal is not an authentication proof. The recorded
Claude fallback must remain visible in any account of client evidence.

Draft release note: live whole-program push, typed state migration, explicit
refusals, offline rehearsal and folder-only restart are implemented in prior
milestones. This preparation does not establish the final drill, production pause
target or agent success rate. Release documentation and the build-input comparison
must be completed against the eventual frozen candidate before Robert's decision.

## Integration verification

`build/m6-integration-checks.txt`: GCC, Clang and sanitizer suites passed. The
combined run ended `check.sh finished: fail` after the parent edited `ci/check.sh`
while Bash was still reading it; drivers reached their final speed check, then
the changed file offset produced a syntax error. `bash -n ci/check.sh` passed.
This operator-caused failed run is retained. The separate drivers/static rerun
ended `check.sh finished: pass`, with no missing-tool skips, in
`build/m6-drivers-static-rerun.txt`. Relevant seeded fuzz targets passed:
upgrade_pages 62,144 executions in 6 seconds (seed 20261110); protocol_server
12 executions in 36 seconds (seed 20261111). Each requested five seconds; the
last iteration can exceed the requested duration. Initial empty-corpus attempts
from an incorrect corpus path remain recorded separately. Timing-enabled
ASan/UBSan production activation also passed its 10-job/one-lease control.
