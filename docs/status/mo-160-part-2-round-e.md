# MO-160 part 2, Round E: listener and robustness

30 October 2026. PR https://github.com/robertguss/mo/pull/262 remains draft.
Implementation and tests: `27d4ea5eef947500b11735250e840ebca5750831`. The measurements ran on the identical source
before that commit was created. The archived before/after manifests match, and
`sha256sum -c` against the committed source passes. The recorded starting HEAD
is `ae4372e67cb0647a5be81f72be68a7d782cfad50`.

All five items are fixed in that implementation commit:

1. `SO_REUSEADDR` is set before bind. `test_listener.c` reopens the same fixed port
   after a served exchange and server-first shutdown, for IPv4 and available IPv6.
   It failed on the original listener and passes with the fix.
2. Retryable accept errors return AGAIN. `test_listener_faults.c` injects
   ECONNABORTED, EPROTO, ENETDOWN, ENOPROTOOPT, EHOSTDOWN, ENONET where available,
   EHOSTUNREACH, EOPNOTSUPP, ENETUNREACH, ENOBUFS, ENOMEM and EINTR once each.
   Every poll stays OK and the queued request receives HTTP 200 with the exact ping
   result. Returning IO instead makes the test fail. Injection uses compile-time
   substitution of accept/accept4, with no production runtime hooks.
3. Deleted PS_ABANDONED and its unreachable branches. This is smaller than adding
   disconnect monitoring and preserves services.md's requirement to retain slots
   through pending replies. Service completion and durable updates still occur;
   a lost updating reply has the documented unknown effect for the client.
4. Pre-auth scan/authentication resource failures send HTTP 413 with an empty body,
   consistently with adjacent plan/feed resource failures. services.md explicitly
   gives 413 for node refusals but does not separately specify a general pre-auth
   allocation-failure status; this change follows the existing resource response.
   `test_protocol_server_faults.c` injects failures at the first four request invokes
   and the first scratch allocation, verifies empty 413, no admission, and recovery
   on the next request. The invoke tests failed on the original implementation.
5. Available PS_BODY reads are coalesced within the existing 1 KiB input buffer
   before calling HttpBody.feed. The loop ends on a full buffer or non-OK read;
   each successful iteration consumes at least one byte. No memory or work allowance
   changed. `test_protocol_server.c` covers 16 KiB bodies at read quanta 1 and 2,
   exact ping responses and the existing half-work threshold. Disabling coalescing
   makes the test fail. The archived runner adds both plain and escaped 16 KiB body
   cases at both quanta. These quanta apply to each listener read with all input
   available; this does not claim packet timing cannot affect work.

Plan: contract “MO-160 listener and client hookup” and “MO-160 measurement
accounting (D317 and D329)” in services.md; listener precedent `779f5158`.
Only host listener setup/error classification, tools request reading/state cleanup,
compile-time fault build rules, their tests and status evidence change. No Core,
bytecode, primitive or capability-operation contract changes. No open product
questions were introduced. The supervisor-event/event-sink behavior is untouched.

Fresh-context review followed review-change. Its ENONET portability finding is
fixed with conditional compilation; the test's accept4 shim is also Linux-only.
No further scoped correctness findings. Red/green logs and the final gate log are
in [the evidence archive](mo-160-part-2-round-e.tar.gz). Module dependency and library-map checks pass.
Clang, sanitizers, Lean, bison, clang-format, clang-tidy and fuzz were not run here;
no tools were installed. CBMC was not run. No missing-tool skips in gcc/drivers.

Gate: `MAKEFLAGS=-j4 CI=1 bash ci/check.sh gcc drivers` (passed).

The adjusted archived runner builds with GCC 13.3.0, strict warnings and `-O2 -g
-pthread`. All 31 matrix cases and all eight catalog modes exit zero with zero
semantic and headroom failures. Timing observations are retained in the raw TSVs;
no numerical wall-time acceptance threshold or native gas accounting was added.

| D329 observation | Measured | Ceiling | 2x headroom threshold where applicable |
|---|---:|---:|---:|
| Header block | 1,024 B | 1,024 B | — |
| Decoded body, escaped and duplicate cases | 16,384 B | 16,384 B | — |
| Complete JSON-RPC response body | 32,768 B | 32,768 B | — |
| Encoded ingress including headers | 24,576 B | 24,576 B | — |
| Preauthentication work | 482,877 | 1,000,000 | 500,000 |
| Preauthentication memory | 75,862 B | 1,048,576 B | 524,288 B |
| Request work | 15,709,453 | 32,000,000 | 16,000,000 |
| Request memory | 3,785,074 B | 16,777,216 B | 8,388,608 B |
| Native memory (included in request accounting) | 1,293,739 B | 16,777,216 B request allowance | 8,388,608 B |
| Eight retained replies, conservative concurrent bound | 13,090,128 B | 134,217,728 B (8 × request allowance) | 67,108,864 B |
| Catalog memory, largest successful catalog | 12,664,285 B | 67,108,864 B | 33,554,432 B |
| Header fields | 64 | 64 | — |
| Token bytes | 256 | 256 | — |
| Value depth | 60 accepted; 61 refused | 60 | — |
| Chunk count | 1,024 | 1,024 | — |
| Chunk size line | 8,192 B | 8,192 B | — |
| Concurrent connections | 8 | 8 | — |
| Catalog reachable type nodes | 4,096 | 4,096 | — |
| Catalog type edges | 16,384 | 16,384 | — |
| Tools per page | 32 (33 tools split 32+1) | 32 | — |
| Worst escaped request ID | 128 decoded B (770 B JSON string) | 128 decoded B | — |
| Byte-fitting Set / Map payload nodes | 2,487 / 4,349 | 16,384 each | — |
| Maximum fitting input schema | 31,230 B | 262,144 B schema; 32,768 B enclosing response | — |
| Schema refusal control | 262,144 B schema; 263,682 B enclosing response | 262,144 B schema; 32,768 B response | — |


The new body cases are included below. Each heading gives its unchanged ceiling
and 2× headroom threshold; these apply separately to every row. HTTP and admissions
are exact expected outcomes. All semantic and headroom flags equal 1.

| Case | HTTP | Admissions | Work (32M; 2× 16M) | Request B (16,777,216; 2× 8,388,608) | Preauth work (1M; 2× 500k) | Preauth B (1,048,576; 2× 524,288) |
|---|---:|---:|---:|---:|---:|---:|
| header1024_fields64_token256_bytewise | 200 | 0 | 1056750 | 117143 | 482877 | 75862 |
| header1024_fields64_token256_bulk | 200 | 0 | 887955 | 117143 | 314082 | 75862 |
| unauthenticated | 401 | 0 | 41126 | 42758 | 41126 | 42758 |
| original_minimal_values256k_rejected_body_byte_ceiling | 413 | 0 | 178544 | 47686 | 91084 | 47686 |
| original_payload_nodes16384_rejected_body_byte_ceiling | 413 | 0 | 178544 | 47686 | 91084 | 47686 |
| original_payload_nodes16385_rejected_body_byte_ceiling | 413 | 0 | 178544 | 47686 | 91084 | 47686 |
| original_worst_escapes256k_rejected_body_byte_ceiling | 413 | 0 | 178544 | 47686 | 91084 | 47686 |
| worst_escapes_exact_body_limit16k | 200 | 1 | 2625360 | 743408 | 90928 | 47670 |
| worst_escapes_body16k_bytewise | 200 | 1 | 2689607 | 743408 | 157467 | 47670 |
| worst_escapes_body16k_twobyte | 200 | 1 | 2655947 | 743408 | 123807 | 47670 |
| plain_body16k_bytewise | 200 | 1 | 1734747 | 1138625 | 157467 | 47670 |
| plain_body16k_twobyte | 200 | 1 | 1701087 | 1138625 | 123807 | 47670 |
| value_depth60 | 200 | 1 | 463418 | 1349921 | 90772 | 47654 |
| value_depth61 | 200 | 0 | 448765 | 1105006 | 90772 | 47654 |
| original_set_duplicate_end_nodes16384_rejected_body_byte_ceiling | 413 | 0 | 178544 | 47686 | 91084 | 47686 |
| original_map_duplicate_end_nodes16384_rejected_body_byte_ceiling | 413 | 0 | 178242 | 47670 | 90928 | 47670 |
| set_duplicate_end_max_byte_fit | 200 | 0 | 4343853 | 862690 | 90928 | 47670 |
| set_max_byte_fit_unique_control | 200 | 1 | 4362140 | 862690 | 90928 | 47670 |
| map_duplicate_end_max_byte_fit | 200 | 0 | 5584839 | 1102850 | 90928 | 47670 |
| map_max_byte_fit_unique_control | 200 | 1 | 5603126 | 1102850 | 90928 | 47670 |
| response_text_empty_baseline | 200 | 1 | 213740 | 89961 | 90098 | 47638 |
| response_text_max_wire_fit | 200 | 1 | 7865823 | 1636266 | 90102 | 47638 |
| response_text_one_past_wire_fit | 200 | 1 | 7866089 | 1636337 | 90102 | 47638 |
| response_returned_text32k_fallback | 200 | 1 | 5343932 | 3129716 | 90102 | 47638 |
| response_returned_nul_text32k_fallback | 200 | 1 | 7834255 | 3785074 | 90101 | 47638 |
| response_many_empty_baseline | 200 | 1 | 212990 | 89982 | 89937 | 47622 |
| response_many_max_wire_fit | 200 | 1 | 5727565 | 1781576 | 89940 | 47622 |
| response_many_one_past_wire_fit | 200 | 1 | 5729052 | 1781725 | 89940 | 47622 |
| response_former_native_nodes16384_fallback | 200 | 1 | 197274 | 89961 | 90098 | 47638 |
| chunks1024_max_ingress_including_headers_line8192 | 200 | 1 | 15709453 | 483259 | 92146 | 47750 |
| concurrent8 | 200 | 8 | 7865823 | 1636266 | 90102 | 47638 |

| Case | Native B (request ceiling 16,777,216; 2× 8,388,608) | Concurrent B (134,217,728; 2× 67,108,864) | Catalog B (67,108,864; 2× 33,554,432) |
|---|---:|---:|---:|
| header1024_fields64_token256_bytewise | 77227 | 117143 | 414697 |
| header1024_fields64_token256_bulk | 77227 | 117143 | 414697 |
| unauthenticated | 36267 | 42758 | 414697 |
| original_minimal_values256k_rejected_body_byte_ceiling | 36267 | 47686 | 414697 |
| original_payload_nodes16384_rejected_body_byte_ceiling | 36267 | 47686 | 414697 |
| original_payload_nodes16385_rejected_body_byte_ceiling | 36267 | 47686 | 414697 |
| original_worst_escapes256k_rejected_body_byte_ceiling | 36267 | 47686 | 414697 |
| worst_escapes_exact_body_limit16k | 138667 | 743408 | 414697 |
| worst_escapes_body16k_bytewise | 138667 | 743408 | 414697 |
| worst_escapes_body16k_twobyte | 138667 | 743408 | 414697 |
| plain_body16k_bytewise | 146795 | 1138625 | 414697 |
| plain_body16k_twobyte | 146795 | 1138625 | 414697 |
| value_depth60 | 1293739 | 1349921 | 414697 |
| value_depth61 | 1052075 | 1105006 | 414697 |
| original_set_duplicate_end_nodes16384_rejected_body_byte_ceiling | 36267 | 47686 | 414697 |
| original_map_duplicate_end_nodes16384_rejected_body_byte_ceiling | 36267 | 47670 | 414697 |
| set_duplicate_end_max_byte_fit | 174363 | 862690 | 414697 |
| set_max_byte_fit_unique_control | 174363 | 862690 | 414697 |
| map_duplicate_end_max_byte_fit | 180971 | 1102850 | 414697 |
| map_max_byte_fit_unique_control | 180971 | 1102850 | 414697 |
| response_text_empty_baseline | 81323 | 89961 | 414697 |
| response_text_max_wire_fit | 101833 | 1636266 | 414697 |
| response_text_one_past_wire_fit | 101835 | 1636337 | 414697 |
| response_returned_text32k_fallback | 101845 | 3129716 | 414697 |
| response_returned_nul_text32k_fallback | 265685 | 3785074 | 414697 |
| response_many_empty_baseline | 81323 | 89982 | 414697 |
| response_many_max_wire_fit | 101829 | 1781576 | 414697 |
| response_many_one_past_wire_fit | 101839 | 1781725 | 414697 |
| response_former_native_nodes16384_fallback | 81323 | 89961 | 414697 |
| chunks1024_max_ingress_including_headers_line8192 | 89563 | 483259 | 414697 |
| concurrent8 | 101833 | 13090128 | 414697 |

| Catalog mode | Result (all exit 0) |
|---|---|
| catalog0 | 4,096 nodes / 16,384 edges / 32 tools; 32 one-tool pages, all exact |
| catalog1 | 33 tiny tools; pages 32+1, exact bodies 27,526 / 903 B |
| catalog2 | 6,000-field schema: 202,041 B input schema, 203,579 B page vs 32,768 B response ceiling; expected startup refusal via Mcp.prepare None |
| catalog3 | 262,144 B schema, 263,682 B page vs 32,768 B response ceiling; expected startup refusal via None |
| catalog4 | 31,230 B input schema gives exact 32,768 B response; accepted |
| catalog5 | 368 B input-schema control accepted; response body 1,137 B |
| catalog6 | 31,231 B schema gives 32,769 B response vs 32,768 B ceiling; expected startup refusal via None |
| catalog7 | 33 byte-limited tools; pages 18+15, exact bodies 31,120 / 25,920 B |

Rejected catalogs returned None without work/memory exhaustion; observed peak
telemetry is unavailable on refusal. For those cases the recorded preallocation
bound was 26,976 B arena usage + 67,081,888 B heap allowance = 67,108,864 B catalog
allowance; process metadata is excluded. Catalog preparation is separate from
per-request work. Its largest measured work was 41,010,043 (catalog3); no new
catalog-work allowance is asserted.


The conservative concurrent sum remains 13,090,128 B; the independent sum of
protocol-process peaks is 13,062,136 B, with final service state exactly 8.
These sum lifetime peaks rather than simultaneous RSS. Compiled images, existing
service allowance and SQLite storage remain outside transport-memory claims.
Original node-ceiling bodies exceed the 16 KiB byte ceiling; unchanged pure
fixtures retain exact node-boundary coverage. Rejected catalogs have no observed
peak telemetry; their preallocation bound and absence of exhaustion are recorded
in the raw logs. No catalog-work allowance is asserted.

The tightest 2× margins are request work: 290,547 below 16,000,000 (1.816% of
threshold), and pre-auth work: 17,123 below 500,000 (3.425%). Request memory has
4,603,534 B spare, catalog memory 20,890,147 B, and concurrent memory 54,018,736 B.
Byte ceilings are intentionally exercised exactly and have zero spare bytes.

The evidence archive contains the expanded harness, adjusted runner, complete raw
matrix and all eight catalog TSVs/logs, metadata, source manifests, source
verification, gate, focused tests and red proofs. The harness and adjusted runner
are also copied into `/tmp/mo160-handoff`.

No limits, schemas, expected behavior, code comments, CLI commands, approved
D316–D322/D324–D330 decisions, PR body, draft state or Notion pages changed.
Nothing was merged, rebased, force-pushed or deleted.

Source-manifest SHA-256: `e2534e51399be18c35eeee55582b8328f05a46afcf572f5792c71aa42796fe3d`.

Evidence archive SHA-256: `d8870254142553574c2c2114b77738df8172323e0cbb65a1e8868ed4202ac17b`
(36 files). The gate's existing codegen corpus exclusions were four
lowering-impossible inputs and 39 programs not checked. These are distinct from
missing-tool skips; none of the requested groups was skipped. The existing
12-sample 0.1-second check-time gate passed; it is not a transport wall-time
acceptance threshold.
