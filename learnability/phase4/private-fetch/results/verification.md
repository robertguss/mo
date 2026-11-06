# U9 local verification, 6 November 2026

Only PR6/U9 is implemented. The starting tree was clean at merged U8
`69b3b7197da286aef31f539c62f6b9eb86380940`, with merged U7 `9ceb11ac` and U3 `5240af58`
in its ancestry. Foundation tests and contracts were inspected before implementation.
No commit, push, PR, external publication or merge was performed. The prohibited private evidence
directory was not read or copied.

## Completed checks

Implementation files stayed frozen during authoritative builds. Long check groups were started
detached and waited for in the same implementation turn. Every final group below ended with
`check.sh finished: pass`; no required tool was skipped.

| Command or check | Evidence |
| --- | --- |
| `MAKEFLAGS=-j2 bash ci/check.sh gcc` | `build/u9-gcc-final-1.txt`; full GCC suite |
| `MAKEFLAGS=-j2 bash ci/check.sh clang sanitize` | `build/u9-clang-sanitize-final-1.txt`; full Clang and ASan/UBSan suites |
| `CI=1 MAKEFLAGS=-j1 TLA2TOOLS_JAR=/tmp/mo-u9-tla2tools.jar bash ci/check.sh static models` | `build/u9-static-models-final-1.txt`; grammar, dependencies, library map, fuzz seeds, clang-format 14.0.6, tidy and pinned TLC |
| `CI=1 MO_ALL_DRIVERS=1 MAKEFLAGS=-j2 bash ci/check.sh drivers` | `build/u9-drivers-final-1.txt`; GCC debug, Clang debug and Clang sanitizer |
| `python3 ci/check_fetch.py MO`, repeated after strengthening acceptance assertions on all three builds | `build/u9-fetch-identities-final-{gcc-debug,clang-debug,clang-sanitize}.txt`; exact committed Text, known Text schema, program/build/service/generation/both sequences, mode and all supported loopback names |
| `python3 ci/check_fetch.py build/gcc/debug/mo --expiry` | `build/u9-fetch-expiry-final-1.txt`; actual idle expiry, cleanup and fresh retry |
| Each build's `tests/test_format.test --files runtime/mcp-fetch.mo` | `build/u9-reader-format-final-1.txt`; the new untracked reader was checked explicitly |
| Stateful `fuzz_fetch`, copied corpus, `-max_total_time=15 -timeout=5 -rss_limit_mb=512` | `build/u9-fuzz-fetch-final-1.txt`; 44 runs in 16 seconds, ASan/UBSan |
| `fuzz_cli`, copied corpus, `-max_total_time=10 -timeout=5 -rss_limit_mb=512` | `build/u9-fuzz-cli-final-1.txt`; 32,020 runs in 11 seconds, ASan/UBSan |
| `bash cbmc/prove.sh export_page_matches mo_export_page_bounds mo_export_schema`, then the same names with `--reach` | `build/u9-cbmc-final-1.txt`, `build/u9-cbmc-reach-final-1.txt`; all proved and every block reached |
| `python3 learnability/phase4/private-fetch/sizing.py build/gcc/debug/mo build/u9-sizing-maximum-verified.json 16767505` | [Maximum sizing](sizing-maximum.json); exit 0 and independent oracle passed |
| `python3 learnability/phase4/private-fetch/client_probe.py build/gcc/debug/mo build/u9-native-smoke-final` | [Native client](native-client.json), [local command observations](native-workflow.json); exit 0, mode 0600, MOEX magic |

All-build drivers include the existing operator, export, CLI golden/guide, recovery and formatter
checks. All 12 normal CLI timing samples stayed below 0.1 seconds; the largest best-of-five
observation was 0.0983 seconds. The final acceptance-only change added assertions and loopback
positive controls after the complete driver group; its three-build rerun passed without any
implementation change. Subsequent changes only recorded this evidence in nonembedded documents.

The compiled peer test refuses non-loopback resolution and actual peers with zero sends, and
injects SIGTERM immediately after temporary creation. The compiled deadline test interrupts
five-second initial-lookup and fsync stalls under a two-second budget, requires completion
below 3.5 seconds and checks that no final or temporary name survives. These tests passed in
all compiler/sanitizer suites. Socket acceptance also covers replacement, final-page release,
cancellation after a valid page, middle/final corruption, exact cleanup, fresh retry, differing
durable/encoded sequences in both orderings, proxy bypass and malformed wire/metadata refusals.

## Resource and proof limits

Sizing used GCC 13.3.0 debug on 8-vCPU Linux, with executable SHA-256 recorded in the result.
It verified a 16,777,216-byte MOSV state and 16,777,405-byte MOEX artifact across 2,049 pages
in 532.07 seconds. The independent oracle follows the specified single-constructor Choice
layout, answer 42 and 16,767,505 repeated Text bytes; it also checks schema and artifact SHA-256
and runtime build identity. The final executable digest still matched the measured binary.

Separate service/fetch RSS high-water marks were 135,928/58,972 KiB (their sum is 190.3 MiB,
not a synchronized combined-RSS observation). Pin bytes included 32 bytes of allocator overhead:
16,777,437 during the fetch, zero afterward. Reported diagnostic scratch peaked at 88,222 bytes.
The JSON also records service heap use, peak, limit and stack capacity. One pending socket
requests 33,792 bytes of receive buffering; Linux reported 67,584 bytes. One wire buffer and
a separately limited 16 MiB parser heap complete the bounded transport/parser allocations.
The full process measurements include compilation and parser costs. They are one fixture on
one host, not universal process-RSS or latency guarantees. Raw artifacts remain local mode 0600.

The new CBMC proof uses unconstrained metadata, both sequence fields and 16-byte IDs, with
17 unwinds for identity initialization/comparison. It proves actual admission/coverage
arithmetic, not SHA correctness, memory copying, networking, signals or filesystem behavior.
The existing schema proof models hashing and checks bounded spans. Fuzzing is finite empirical
evidence. TLC's generation/release negative controls found their expected counterexamples;
its fairness and finite-state assumptions remain in `models/README.md`. No formal model or
Core semantics changed, so Lean was outside this unit's verification scope.

The monotonic deadline assumes scheduling and interruptible POSIX calls. SIGKILL, machine
crashes, uninterruptible kernel waits and filesystem failures can prevent cleanup. Checksums
establish consistency; they do not authenticate a malicious local service. Pin creation,
final-page completion/release and verified file publication remain separate events.

## Independent review and retained failures

The completed CE review receipt is retained at
`/tmp/compound-engineering-1000/ce-code-review/20261106-000851-713d3ca3/review.json`.
It records ten local perspectives plus independent finding validation. Its original verdict
was **Not ready**, describing an earlier snapshot. Independent simplification and read-only
follow-ups informed the fixes; the last focused follow-up found no new concrete defect in
lookahead/deadline changes, while resource/final-check evidence was still pending then.
The passing results above close those remaining evidence gates. This reconciliation does not
rewrite that original receipt or claim a second full review. Cross-provider review did not run
because the tool lifecycle required ending the implementation turn; no result was incorporated.

| Original confirmed finding | Final resolution and evidence |
| --- | --- |
| Stateful/lifecycle/resource verification incomplete | Shared typed consumer, sequence fuzz/corpora, resealed inner-framing tests, real lifecycle/expiry/retry acceptance, scoped proof/reach and corrected maximum-size oracle all passed |
| Complete deadline missing | Numeric-only resolution, timer armed before initial lookup, interruptible completion and late-link cleanup; compiled lookup/fsync controls passed |
| Guide/discovery advertised fetch unavailable | Embedded guide, discovery description and guide assertions corrected; all-build CLI checks and native smoke passed |
| Replacement refusal discarded | Whitelisted replaced/released reader status and native retry diagnostics; lifecycle assertions passed |
| Handlers installed after temporary creation | Both handlers precede open; injected SIGTERM leaves no temporary file or transmission |
| Corrupt-first-page test could pass on later connection refusal | Listener stays available and records continuation; malformed first pages require exactly one request; valid complete controls also pass |

Earlier failures remain evidence:

- The reader red test failed compilation before its API existed. Compiler/parser fixture errors
  and focused development failures remain in `/tmp/mo-u9-*` logs.
- Three broad earlier runs remain `build/u9-check-results-{1,2,3}.txt`. They were deliberately
  interrupted before edits. Run 1 lacks a completion marker; runs 2/3 end in fail. None is a pass.
- Confidential response-buffer growth exhausted its small allowance; allocating the named
  bounded wire buffer once fixed it. Simplification also found literal-pointer subtraction,
  JSON type coercion, exact-umask permissions and publication/cleanup reporting issues.
- Serial sizing timed out after 600.48, 600.05 and 600.10 seconds, including a low-traffic
  control; `/tmp/mo-u9-size-*` retains results/logs. The prediction preceding one-page lookahead
  remains `/tmp/mo-u9-lookahead-prediction.txt`.
- Lookahead CLI fetches completed in 497.45, 499.56 and 522.48 seconds, but the sizing oracle
  misread the nominal struct header. These harness failures remain
  `build/u9-sizing-{lookahead,complete,maximum}-result.txt`. The oracle was corrected from the
  saved-state specification/lowering, independently checked on a small fixture, then passed
  the maximum fixture. A successful CLI alone was not counted as successful sizing.
- Native attempt 1 had duplicate model flags; attempt 2 lacked shell sandbox network access.
  Both remain beside the successful attempts under `build/u9-native-smoke*`.

Native smoke used Codex 0.166.0, GPT-6.1 Sol/high, explicit tool approval and shell network
access. It observed discovery/routing and the supplied host-local workflow. It does not establish
unaided diagnostic effectiveness. The pilot remains 30 attempts, 29 completed grades, pilot7
interrupted, four uncalibrated variants and no independent baseline. Prospective evaluation
approval still gates U6 collection. Failure capture/export remains U4 and is unavailable.
