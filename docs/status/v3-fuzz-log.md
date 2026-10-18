# V3 Hour-Long Fuzzing Log (`MO-53`)

Run environment: Ubuntu 24.04 container `mo-lw5-ci-20261016` (`/mo-v3`), Clang 18 libFuzzer with AddressSanitizer and UndefinedBehaviorSanitizer (`-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all`), low priority (`nice -n 19`), 3,600 seconds per target.

## Summary of Completed 1-Hour Targets (0 Crashes)

| Target | Window (UTC) | Status | Executed Units | Avg Exec/s | New Units Added | Peak RSS (MB) | Crashes / Artifacts |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `fuzz_bodies` | `03:13:52Z` – `04:13:53Z` | `0` | 155,079 | 43 | 4,270 | 197 | 0 |
| `fuzz_bytecode_io` | `04:13:53Z` – `05:13:54Z` | `0` | 28,587,983 | 7,938 | 2,803 | 671 | 0 |
| `fuzz_bytecode_io_large` | `05:13:54Z` – `06:13:55Z` | `0` | 4,040,200 | 1,121 | 2,206 | 787 | 0 |
| `fuzz_bytecode_text` | `06:13:55Z` – `07:13:56Z` | `0` | 2,776,946 | 771 | 7,064 | 824 | 0 |
| `fuzz_bytecode_verify` | `07:13:56Z` – `08:13:57Z` | `0` | 98,139,401 | 27,253 | 3,168 | 626 | 0 |
| `fuzz_check` | `08:13:57Z` – `09:13:58Z` | `0` | 4,651,625 | 1,291 | 22,612 | 1,026 | 0 |
| `fuzz_cli` | `09:13:58Z` – `10:13:59Z` | `0` | 7,146,251 | 1,984 | 6,939 | 196 | 0 |
| `fuzz_codegen` | `10:13:59Z` – `11:14:01Z` | `0` | 3,021,058 | 838 | 3,575 | 933 | 0 |
| `fuzz_core_check` | Started `11:14:01Z` | Not finished (rerun below) | — | — | — | — | 0 |

Total executions across the 8 completed 1-hour targets: **148,518,543** runs with **0 crashes** (`build/fuzz/artifacts/` is empty).

## Second Session: Remaining 15 Targets (0 Crashes)

Run on 2026-10-18 from main `1511a7a` (later commits changed `src/`; the targets were not rebuilt). The first session finished 8 of the 23 runs (22 fuzz targets plus the large-bytecode variant of `fuzz_bytecode_io`). `fuzz_core_check` was still running when that log stopped, so it was rerun from scratch here. This session covers the other 15, so all 23 runs have now had an hour.

Environment: Ubuntu 24.04 cloud container with 4 cores, Clang 18 libFuzzer with AddressSanitizer and UndefinedBehaviorSanitizer (same flags as above), `nice -n 19`, 3,600 seconds per target, four targets at a time. Corpora were seeded by `make fuzz-smoke FUZZ_SECONDS=2 CC=clang` and options match that target's settings in the Makefile. Because four ran at once on four cores, executions per second are lower than a solo run would give.

| Target | Window (UTC, 2026-10-18) | Status | Executed Units | Avg Exec/s | New Units Added | Peak RSS (MB) | Crashes / Artifacts |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `fuzz_core_text` | `01:05:42Z` – `02:05:43Z` | `0` | 403,124 | 111 | 1,511 | 408 | 0 |
| `fuzz_grammar` | `01:05:42Z` – `02:05:43Z` | `0` | 7,660,334 | 2,127 | 19,044 | 413 | 0 |
| `fuzz_format` | `01:05:42Z` – `02:05:43Z` | `0` | 507,379 | 140 | 10,142 | 348 | 0 |
| `fuzz_core_check` | `01:05:42Z` – `02:05:43Z` | `0` | 1,581,941 | 439 | 2,987 | 568 | 0 |
| `fuzz_host_real` | `02:05:43Z` – `03:05:44Z` | `0` | 1,121,960 | 311 | 2,067 | 229 | 0 |
| `fuzz_heap` | `02:05:43Z` – `03:05:44Z` | `0` | 1,277,566 | 354 | 5,296 | 475 | 0 |
| `fuzz_host` | `02:05:43Z` – `03:05:44Z` | `0` | 6,024,512 | 1,673 | 7,279 | 547 | 0 |
| `fuzz_inputs` | `02:05:43Z` – `03:05:44Z` | `0` | 110,504 | 30 | 2,206 | 471 | 0 |
| `fuzz_lower` | `03:05:44Z` – `04:05:45Z` | `0` | 83,582 | 23 | 585 | 175 | 0 |
| `fuzz_prim` | `03:05:44Z` – `04:05:45Z` | `0` | 1,461,036 | 405 | 10,221 | 424 | 0 |
| `fuzz_program` | `03:05:44Z` – `04:05:45Z` | `0` | 62,968 | 17 | 2,891 | 184 | 0 |
| `fuzz_sources` | `03:05:44Z` – `04:05:45Z` | `0` | 12,961,844 | 3,599 | 2,598 | 120 | 0 |
| `fuzz_types` | `04:05:45Z` – `05:05:46Z` | `0` | 61,580,042 | 17,100 | 1,701 | 431 | 0 |
| `fuzz_vm` | `04:05:45Z` – `05:05:46Z` | `0` | 49,902,454 | 13,857 | 3,093 | 497 | 0 |
| `fuzz_syntax` | `04:05:45Z` – `05:05:47Z` | `0` | 124,995 | 34 | 4,539 | 395 | 0 |

Total executions across these 15 targets: **144,864,241** runs with **0 crashes** (`build/fuzz/artifacts/` is empty). Across both sessions: **293,382,784** runs over 23 one-hour runs, no crash.

## Raw `summary.log`

```text
[2026-10-16T03:13:52Z] Starting fuzz_bodies for 3600s
[2026-10-16T04:13:53Z] Finished fuzz_bodies with status 0
Done 155079 runs in 3601 second(s)
stat::number_of_executed_units: 155079
stat::average_exec_per_sec:     43
stat::new_units_added:          4270
stat::slowest_unit_time_sec:    0
stat::peak_rss_mb:              197
[2026-10-16T04:13:53Z] Starting fuzz_bytecode_io for 3600s
[2026-10-16T05:13:54Z] Finished fuzz_bytecode_io with status 0
Done 28587983 runs in 3601 second(s)
stat::number_of_executed_units: 28587983
stat::average_exec_per_sec:     7938
stat::new_units_added:          2803
stat::slowest_unit_time_sec:    0
stat::peak_rss_mb:              671
[2026-10-16T05:13:54Z] Starting fuzz_bytecode_io_large for 3600s
[2026-10-16T06:13:55Z] Finished fuzz_bytecode_io_large with status 0
Done 4040200 runs in 3601 second(s)
stat::number_of_executed_units: 4040200
stat::average_exec_per_sec:     1121
stat::new_units_added:          2206
stat::slowest_unit_time_sec:    0
stat::peak_rss_mb:              787
[2026-10-16T06:13:55Z] Starting fuzz_bytecode_text for 3600s
[2026-10-16T07:13:56Z] Finished fuzz_bytecode_text with status 0
Done 2776946 runs in 3601 second(s)
stat::number_of_executed_units: 2776946
stat::average_exec_per_sec:     771
stat::new_units_added:          7064
stat::slowest_unit_time_sec:    0
stat::peak_rss_mb:              824
[2026-10-16T07:13:56Z] Starting fuzz_bytecode_verify for 3600s
[2026-10-16T08:13:57Z] Finished fuzz_bytecode_verify with status 0
Done 98139401 runs in 3601 second(s)
stat::number_of_executed_units: 98139401
stat::average_exec_per_sec:     27253
stat::new_units_added:          3168
stat::slowest_unit_time_sec:    0
stat::peak_rss_mb:              626
[2026-10-16T08:13:57Z] Starting fuzz_check for 3600s
[2026-10-16T09:13:58Z] Finished fuzz_check with status 0
Done 4651625 runs in 3601 second(s)
stat::number_of_executed_units: 4651625
stat::average_exec_per_sec:     1291
stat::new_units_added:          22612
stat::slowest_unit_time_sec:    0
stat::peak_rss_mb:              1026
[2026-10-16T09:13:58Z] Starting fuzz_cli for 3600s
[2026-10-16T10:13:59Z] Finished fuzz_cli with status 0
Done 7146251 runs in 3601 second(s)
stat::number_of_executed_units: 7146251
stat::average_exec_per_sec:     1984
stat::new_units_added:          6939
stat::slowest_unit_time_sec:    0
stat::peak_rss_mb:              196
[2026-10-16T10:13:59Z] Starting fuzz_codegen for 3600s
[2026-10-16T11:14:01Z] Finished fuzz_codegen with status 0
Done 3021058 runs in 3601 second(s)
stat::number_of_executed_units: 3021058
stat::average_exec_per_sec:     838
stat::new_units_added:          3575
stat::slowest_unit_time_sec:    0
stat::peak_rss_mb:              933
[2026-10-16T11:14:01Z] Starting fuzz_core_check for 3600s
```

## Raw `summary.log`, second session

```text
[2026-10-18T01:05:42Z] Starting fuzz_core_text for 3600s
[2026-10-18T01:05:42Z] Starting fuzz_format for 3600s
[2026-10-18T01:05:42Z] Starting fuzz_core_check for 3600s
[2026-10-18T01:05:42Z] Starting fuzz_grammar for 3600s
[2026-10-18T02:05:43Z] Finished fuzz_core_text with status 0
Done 403124 runs in 3601 second(s)
stat::number_of_executed_units: 403124
stat::average_exec_per_sec:     111
stat::new_units_added:          1511
stat::slowest_unit_time_sec:    0
stat::peak_rss_mb:              408
[2026-10-18T02:05:43Z] Starting fuzz_heap for 3600s
[2026-10-18T02:05:43Z] Finished fuzz_grammar with status 0
Done 7660334 runs in 3601 second(s)
stat::number_of_executed_units: 7660334
stat::average_exec_per_sec:     2127
stat::new_units_added:          19044
stat::slowest_unit_time_sec:    0
stat::peak_rss_mb:              413
[2026-10-18T02:05:43Z] Starting fuzz_host for 3600s
[2026-10-18T02:05:43Z] Finished fuzz_format with status 0
Done 507379 runs in 3601 second(s)
stat::number_of_executed_units: 507379
stat::average_exec_per_sec:     140
stat::new_units_added:          10142
stat::slowest_unit_time_sec:    0
stat::peak_rss_mb:              348
[2026-10-18T02:05:43Z] Starting fuzz_host_real for 3600s
[2026-10-18T02:05:43Z] Finished fuzz_core_check with status 0
Done 1581941 runs in 3601 second(s)
stat::number_of_executed_units: 1581941
stat::average_exec_per_sec:     439
stat::new_units_added:          2987
stat::slowest_unit_time_sec:    0
stat::peak_rss_mb:              568
[2026-10-18T02:05:43Z] Starting fuzz_inputs for 3600s
[2026-10-18T03:05:44Z] Finished fuzz_host_real with status 0
Done 1121960 runs in 3601 second(s)
stat::number_of_executed_units: 1121960
stat::average_exec_per_sec:     311
stat::new_units_added:          2067
stat::slowest_unit_time_sec:    0
stat::peak_rss_mb:              229
[2026-10-18T03:05:44Z] Starting fuzz_lower for 3600s
[2026-10-18T03:05:44Z] Finished fuzz_heap with status 0
Done 1277566 runs in 3601 second(s)
stat::number_of_executed_units: 1277566
stat::average_exec_per_sec:     354
stat::new_units_added:          5296
stat::slowest_unit_time_sec:    0
stat::peak_rss_mb:              475
[2026-10-18T03:05:44Z] Starting fuzz_prim for 3600s
[2026-10-18T03:05:44Z] Finished fuzz_host with status 0
Done 6024512 runs in 3601 second(s)
stat::number_of_executed_units: 6024512
stat::average_exec_per_sec:     1673
stat::new_units_added:          7279
stat::slowest_unit_time_sec:    0
stat::peak_rss_mb:              547
[2026-10-18T03:05:44Z] Starting fuzz_program for 3600s
[2026-10-18T03:05:44Z] Finished fuzz_inputs with status 0
Done 110504 runs in 3601 second(s)
stat::number_of_executed_units: 110504
stat::average_exec_per_sec:     30
stat::new_units_added:          2206
stat::slowest_unit_time_sec:    0
stat::peak_rss_mb:              471
[2026-10-18T03:05:44Z] Starting fuzz_sources for 3600s
[2026-10-18T04:05:45Z] Finished fuzz_lower with status 0
Done 83582 runs in 3601 second(s)
stat::number_of_executed_units: 83582
stat::average_exec_per_sec:     23
stat::new_units_added:          585
stat::slowest_unit_time_sec:    0
stat::peak_rss_mb:              175
[2026-10-18T04:05:45Z] Starting fuzz_syntax for 3600s
[2026-10-18T04:05:45Z] Finished fuzz_prim with status 0
Done 1461036 runs in 3601 second(s)
stat::number_of_executed_units: 1461036
stat::average_exec_per_sec:     405
stat::new_units_added:          10221
stat::slowest_unit_time_sec:    0
stat::peak_rss_mb:              424
[2026-10-18T04:05:45Z] Starting fuzz_types for 3600s
[2026-10-18T04:05:45Z] Finished fuzz_program with status 0
Done 62968 runs in 3601 second(s)
stat::number_of_executed_units: 62968
stat::average_exec_per_sec:     17
stat::new_units_added:          2891
stat::slowest_unit_time_sec:    0
stat::peak_rss_mb:              184
[2026-10-18T04:05:45Z] Starting fuzz_vm for 3600s
[2026-10-18T04:05:45Z] Finished fuzz_sources with status 0
Done 12961844 runs in 3601 second(s)
stat::number_of_executed_units: 12961844
stat::average_exec_per_sec:     3599
stat::new_units_added:          2598
stat::slowest_unit_time_sec:    0
stat::peak_rss_mb:              120
[2026-10-18T05:05:46Z] Finished fuzz_types with status 0
Done 61580042 runs in 3601 second(s)
stat::number_of_executed_units: 61580042
stat::average_exec_per_sec:     17100
stat::new_units_added:          1701
stat::slowest_unit_time_sec:    0
stat::peak_rss_mb:              431
[2026-10-18T05:05:46Z] Finished fuzz_vm with status 0
Done 49902454 runs in 3601 second(s)
stat::number_of_executed_units: 49902454
stat::average_exec_per_sec:     13857
stat::new_units_added:          3093
stat::slowest_unit_time_sec:    0
stat::peak_rss_mb:              497
[2026-10-18T05:05:47Z] Finished fuzz_syntax with status 0
Done 124995 runs in 3601 second(s)
stat::number_of_executed_units: 124995
stat::average_exec_per_sec:     34
stat::new_units_added:          4539
stat::slowest_unit_time_sec:    0
stat::peak_rss_mb:              395
```
