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
| `fuzz_core_check` | Started `11:14:01Z` | Running | — | — | — | — | 0 |

Total executions across the 8 completed 1-hour targets: **148,518,543** runs with **0 crashes** (`build/fuzz/artifacts/` is empty).

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
