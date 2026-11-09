# Pause margin follow-up

User authorized runtime optimization and remeasurement on 2026-11-09, preserving
the existing <=0.5 second maximum and all prior failed measurements. No merge,
release or phase closure is authorized. Original debug maximum remains 0.554238s.

Before changes: prediction is that catalog construction dominates avoidable
boundary time. Retained debug-0 protocol_catalog is 199718277 ns of 485798519 ns.
The image key is already cached; it is not a whole-image hash in this path.
No change to measurement boundaries, SQLite durability, limits, confidentiality,
validation, workload (5000 jobs, 2048 bytes, 500 leases), or pass gate is intended.

First locating measurement: add compile-time nested catalog timings, build the
existing GCC timing tree, run exactly one full-size DEVELOPMENT sample. Distinguish
descriptor creation, Mo prepare invocation, and immortal retention. Preserve raw
results; do not count this single sample as the required final five-per-build series.
perf stat could not run: the installed wrapper has no executable for kernel
6.12.113. No package installation was attempted.

Locating sample completed: catalog_prepare 175927987 ns; descriptor 79018 ns;
retention 1790374 ns; total boundary 446181987 ns. Exact state verification passed.
The sample is development only and does not supersede the original five-sample miss.

Next hypothesis, before implementation: Mcp.prepare renders every tool's complete
JSON solely to obtain its byte length. Replace that materialization with an exact
recursive byte counter local to Mcp, preserving Json.to_text's escaping and UTF-8
semantics. Prediction: fewer temporary strings and VM calls reduce catalog_prepare
without changing catalog contents, paging sizes, response limits or validation.
Run existing renderer equivalence examples covering every JSON constructor,
control escapes, quote/backslash and multibyte text; then one full-size development
sample before deciding whether to run the five-per-build confirmation.

The first changed full-size development sample completed with exact state checks:
catalog_prepare 147313948 ns; boundary_to_commit 407532512 ns. This is only one
sample; no repeated-series pass is claimed. Nine byte-counter examples passed.
Two direct `mo run tests/runtime/...` attempts failed E0034 because these fixtures
need the embedded protocol test loader; their logs are retained. The correct
`make .../test_lower.test.run CC=gcc` completed successfully. Add an independent
per-tool sizes-versus-renderer assertion to the existing exact-pagination fixture.
`mo format --check` was an invalid CLI invocation (exit 2), not a format pass.

The updated lower suite completed: `lower: all checks passed`, including the
per-tool renderer-equivalence assertion (build/pause-lower-size-check.txt).
Mo formatting succeeded with `mo format runtime/mcp.mo`; C formatting used
clang-format-14. Final repeated timing series, Clang/sanitizer checks, full GCC
check, independent review and final freeze remain pending. Available disk fell
to 26 MiB while working; no new build or collection is safe at that capacity.
Cache cleanup approval was requested for npm _cacache (3.6 GiB) and pip cache
(109 MiB); no cache has been removed pending the user's answer. All started
measurements and tests finished. The source change remains provisional until
repeated measurement and required checks; no phase closure or release claim.
