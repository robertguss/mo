# MO-160 part 2: GCC/drivers verification, 30 October 2026

PR: https://github.com/robertguss/mo/pull/262 (draft; body and draft state unchanged).
Measured and tested source: `e145ee324029401da8ae9ada4112f68cb773ebf1`.

Merge commit `e145ee324029401da8ae9ada4112f68cb773ebf1` merges `origin/main`
(`0543c3b0`) into the existing branch. The only conflict was in
`docs/status/agent-mistakes.md`; both the MO-160 diagnostic entry and PR #263's
reachable-join entry were retained. Code and tests merged automatically. No
additional implementation fixes were needed. No limits, allowances, schemas,
expected behavior or code comments changed.

The archived runner was extracted into `/tmp/mo160-handoff`; copies of its harness
and runner were placed in `/tmp`, changing only the runner's checkout path.
GCC 13.3.0 built the runtime with the repository's strict warning flags and
`-O2 -g -pthread`. The optimized server test, matrix and all eight catalog modes
exited zero. Each mode reported zero semantic failures and zero headroom failures.
Before/after manifests are identical, SHA-256
`3fbed68f4bb821aa9d7cf2d363cce1e8a3af8682f2f32b381c5dbbb9c0def973`.
They cover the harness, adjusted runner, Makefile, fixture headers and all files
under `src`, `runtime` and `lib`.

| Check group | Result |
|---|---|
| gcc | Passed; no missing-tool skips |
| drivers | Passed; no missing-tool skips |
| clang | Not run here: clang missing; assigned elsewhere |
| sanitize | Not run here by instruction: clang missing; assigned elsewhere |
| static | Not run here: bison, clang-format and clang-tidy missing; assigned elsewhere |
| lean | Not run here: Lean/lake/elan missing, including /opt/elan/bin; assigned elsewhere |
| fuzz | Not run here: clang/libFuzzer missing; assigned elsewhere |
| cbmc | Not run: outside scope; CBMC missing |

Gate command: `MAKEFLAGS=-j4 CI=1 bash ci/check.sh gcc drivers`.
The evidence archive `mo-160-part-2-gcc-drivers.tar.gz` contains the complete gate log and raw measurement TSVs/logs.
Archive SHA-256: `16fa69f3e4f2cd0c7cfd3b9809ee63e96382c0ef0a4c9b61ac1c06e169be86c7` (28 files).
Codegen corpus exclusions: four lowering-impossible inputs skipped and 39 programs
not checked. These are separate from missing-tool skips. All 12 timed samples
passed the existing 0.1-second check-time gate; this is not a native transport
wall-time acceptance threshold.

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

The 27 matrix cases all passed semantic, preauthentication-headroom and
request-headroom assertions. The independent sum of eight retained protocol-process
peaks was 13,062,136 B; final state was exactly 8. Concurrent accounting sums active
lifetime peaks rather than simultaneous-live RSS. Native scratch can include earlier
service activity. Compiled images, existing service allowance and SQLite storage are
outside transport-memory claims.

Original 16,384/16,385-node bodies exceed the 16 KiB body ceiling and receive 413
before parsing; these runs do not prove native admission at the payload-node limit.
The unchanged pure fixtures retain exact payload (16,384) and envelope (64) boundary
coverage. No additional native node-boundary acceptance is claimed.

Response controls produced 32,768 B for 16,315 plain characters; one more character
would require 32,770 B and used the response-limit fallback. The many-element result
fit 3,263 elements / 3,264 native nodes in 32,764 B; one more required 32,774 B and
used the fallback. Worst escaped input used 2,718 decoded value bytes in a 16,384 B
body. Set/Map late duplicates were refused and unique controls succeeded.

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

Request/service-stage maximum was 411,546 µs; maximum poll was 418,489 µs.
Catalog preparation maximum was 1,077,160 µs. Timing overlapped the GCC/drivers gate
and has no numerical acceptance threshold. Raw fake-clock stage_us remains zero;
monotonic timing columns contain measured durations. TSV output_bytes includes HTTP
headers, and the concurrent row sums eight responses; compare JSON-RPC body sizes,
not those raw transport totals, with the 32 KiB response ceiling.

No client-interoperability run or independent code review was performed in this
scoped thread. Other-machine groups, final socket/client verification and independent
review remain part of the broader brief. No product decision blocked this work.
No PR body, draft-state or Notion edits were made, and nothing was merged into main.

Per-case matrix values follow. Full timing, ingress, output, catalog and native
metrics for every matrix and catalog page are preserved in the evidence archive.
Work and memory columns below carry their allowance in the heading.

| Case | HTTP | Admissions | Work / 32,000,000 | Request B / 16,777,216 | Preauth work / 1,000,000 | Preauth B / 1,048,576 |
|---|---:|---:|---:|---:|---:|---:|
| header1024_fields64_token256_bytewise | 200 | 0 | 1146138 | 117143 | 482877 | 75862 |
| header1024_fields64_token256_bulk | 200 | 0 | 887955 | 117143 | 314082 | 75862 |
| unauthenticated | 401 | 0 | 41126 | 42758 | 41126 | 42758 |
| original_minimal_values256k_rejected_body_byte_ceiling | 413 | 0 | 178544 | 47686 | 91084 | 47686 |
| original_payload_nodes16384_rejected_body_byte_ceiling | 413 | 0 | 178544 | 47686 | 91084 | 47686 |
| original_payload_nodes16385_rejected_body_byte_ceiling | 413 | 0 | 178544 | 47686 | 91084 | 47686 |
| original_worst_escapes256k_rejected_body_byte_ceiling | 413 | 0 | 178544 | 47686 | 91084 | 47686 |
| worst_escapes_exact_body_limit16k | 200 | 1 | 2625360 | 743408 | 90928 | 47670 |
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
