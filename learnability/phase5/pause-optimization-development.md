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

User approved clearing npm/pip download caches. Cleanup removed only
/home/exedev/.npm/_cacache and /home/exedev/.cache/pip; free space rose from
73728 to 3956744192 bytes. No installed tool or retained evidence was removed.

Prospective confirmation: exactly five debug and five optimized samples, in
that order, using the unchanged 5000-job/2048-byte/500-lease driver, a separate
build/pause-json-size-confirmation directory and a 1800-second outer timeout.
Both timing binaries are rebuilt from commit 44026e06. This is DEVELOPMENT
confirmation, not release-frozen collection; keep every failure and do not
select the best runs. Required maximum remains 0.5 seconds for each build.

All ten declared confirmation samples completed with exact state verification.
Debug missed the unchanged gate; optimized passed. Every sample is retained in
build/pause-json-size-confirmation/results.json and its sample directories.

optimized: {"count": 5, "min": 0.227962173, "median": 0.244590767, "max": 0.274176731, "sample_stddev": 0.017145278977358326}; meets_margin=True

debug: {"count": 5, "min": 0.428525876, "median": 0.465305024, "max": 0.669134839, "sample_stddev": 0.11397659750706}; meets_margin=False

Debug samples 2 and 3 exceeded the margin across multiple stages. No automatic
replacement or unchanged rerun is justified. The optimization does not close
the pause gate. Latest remote nightly checked during this work succeeded at
814ddd0f70e5fd613130fe87a9cd24cd187af8e6; PR #318 remains open.

Focused independent review requested Claude Opus 5.5/high via T3; returned no
correctness defect, with low/info notes about remaining character-list allocation,
unsummarized nested probes and number-spelling test coverage. No tests were run
by that reviewer. No exact serving-model provenance claim is made from the request.

GCC check at 44026e06 completed: check.sh finished: pass. Clang ordinary build
passed; targeted Clang ASan/UBSan lower and protocol-server tests passed.

Next bounded experiment: existing native Text.replace returns the original value
when no match exists. Compare 1000 short-key and long-description counts using
character walks and 34 native searches. First four fixtures failed E0035 because
module Main did not match their filenames; retained. Four corrected fixtures
completed: long chars 1.754867s, native 0.229598s; short chars 0.124214s, native
0.222875s. These include compilation and concurrent checks, not isolated benchmark
ratios. Keep char walk below 64 bytes, use native search above; test all control
bytes and UTF-8 around this threshold, then exactly one full-size debug sample.
No final-series rerun until a changed implementation has supporting evidence.

The first native-counter test fixture used Text + and failed E0075; preserved in
build/pause-native-size-tests.txt. Corrected interpolation passed all ten test
cases (nine examples and a parameterized renderer comparison). Extended that
comparison to empty suffixes and repeated escapes, covering actual lengths 63/64/65.
The development full-size native-counter sample passed state verification:
catalog_prepare 110720000 ns approximately, boundary_to_commit 379290000 ns
approximately (exact raw records retained in build/pause-native-size-development).

Prospective second confirmation: five debug then five optimized samples, same
5000/2048/500 workload and <=0.5s maximum, all outcomes kept. Directory
build/pause-native-size-confirmation; outer timeout 1800 seconds. Rebuilt both
timing binaries. No concurrent parent build/test workload during measurement.
This is a new implementation, not a replacement of the failed first series.

All ten second-confirmation samples completed, with exact state checks and activation
ordinal 1. Debug maximum 0.392591111 s and optimized maximum 0.222705161 s both
meet the unchanged <=0.5 s margin. This is DEVELOPMENT evidence, not a release
freeze. The original official debug miss and first confirmation miss remain intact.
Sanitized raw timings for all five development/confirmation series are retained in
results/pause-optimization/, with an exact SHA-256 inventory and the additional
runtime/library build-input hashes. All current inputs match those hashes.
The 261-file archive round trip and scans against three private credential
inventories passed; private stores and earlier malformed fixtures stay local.

Final validation of the native-counter candidate: GCC check ended with
`check.sh finished: pass`; ordinary Clang build and targeted Clang ASan/UBSan
lower and protocol-server tests passed. All ten renderer-equivalence examples
passed, as did dependency and formatting checks. The focused capture, replay,
publication, parser, grading and lifecycle regression run passed 225 tests in
114.830 seconds (build/resume-evaluation-focused.txt). No skip is counted as a pass.

A second independent read-only review of the native counter found no defect after
checking the renderer, UTF-8, every control byte, NUL and native replacement
semantics. It did not run tests, verify temporary lifetimes in generated code or
measure the cutoff. These scoped reviews are not whole-phase approval.
