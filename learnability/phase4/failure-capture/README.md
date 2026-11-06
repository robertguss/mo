# Failure capture development evidence

These are implementation checks for retained handler failure captures, not scored diagnostic
observations or evidence of agent effectiveness. The pilot remains 30 attempts, 29 completed
grades, interrupted pilot7, four uncalibrated variants and no independent baseline. No private
pilot directory was read.

The design's capture invariants predicted complete published pairs, unchanged application
recovery, one supervisor count per failure, and a stop after an uncertain store outcome. The
checks below test those predictions. The storage sizing run is a measurement, not a
predeclared performance claim.

## What the kept results show

Every result except `models-final.txt` was recorded on the PR's first commit, before the
review fixes that make damaged diagnostic rows a clean refusal, validate each built message,
escape top-level values and keep a failure export alive across a missing-id request. Before
merge, `bash ci/check.sh gcc` and the `drivers` group run again on the final code; those runs,
not these files, are the merge evidence.

| File | What it shows |
| --- | --- |
| [results/startup-compilers-final.txt](results/startup-compilers-final.txt) | `bash ci/check.sh gcc clang sanitize`: every unit suite under GCC, Clang and ASan+UBSan. Last line `check.sh finished: pass` |
| [results/authoritative-drivers-final.txt](results/authoritative-drivers-final.txt) | `bash ci/check.sh drivers`. It ends `check.sh finished: fail`. Every functional driver passed, including the operator, fetch, export, CLI, task-queue, golden and recovery drivers, but the final timing gate measured `mo check` on `job-queue.mo` at 0.1132 seconds against a 0.1-second limit. It is not a passing group |
| [results/timing-final-alone.txt](results/timing-final-alone.txt) | The timing gate run alone on that build: `job-queue.mo` at 0.1111 seconds, still over |
| [results/check-startup-red.txt](results/check-startup-red.txt), [results/check-startup-green.txt](results/check-startup-green.txt) | The fix for that timing failure. `mo check` no longer hashes the executable. `test_sha256` fails before the change (`check-startup-red.txt`, line 3) and passes after it |
| [results/timing-startup-fixed.txt](results/timing-startup-fixed.txt) | After the fix, all twelve samples are within 0.1 seconds; the slowest is `job-queue.mo` at 0.0746 seconds. The functional drivers were not repeated on this build |
| [results/static-models-final.txt](results/static-models-final.txt) | `bash ci/check.sh static models`: grammar, module dependencies, library map, fuzz corpora, formatting, clang-tidy and the original TLC models. Last line `check.sh finished: pass` |
| [results/models-final.txt](results/models-final.txt) | `python3 ci/check_models.py` on the strengthened `models/Capture.tla`: the passing instance, and one counterexample for each of its six invariants (`models/README.md`) |
| [results/fuzz-final.txt](results/fuzz-final.txt) | `fuzz_service_failure` for 31 seconds: 1,772,318 executions without a crash. Bounded fuzz evidence only |
| [results/proofs-remediated.txt](results/proofs-remediated.txt), [results/reach-remediated.txt](results/reach-remediated.txt) | CBMC proves, and reaches every block of, `slice`, `mo_export_page_bounds`, `mo_export_schema` and `export_page_matches`. The `slice` harness covers length-prefixed slices of at most 32 bytes with any valid cursor and limit, and leaves UTF-8 validation unconstrained. It is not a whole-record or SQLite proof |
| [results/kill-loop-final.txt](results/kill-loop-final.txt) | The recovery kill loop, seed 20261106: 100 kills, 16 of them mid-commit, 1,669 calls, result pass |
| [results/sizing.json](results/sizing.json) | `test_failure_sizing`: three transactions of a maximal pair (16 MiB state, 1,048,064-byte message, 17,825,348 framed bytes) in 0.536 seconds, peak RSS 56,644 KiB from a 21,592 KiB baseline, and a 35,708,928-byte database holding two retained pairs. The payloads are opaque storage fixtures; the figures include native buffers and SQLite overhead, and are specific to this VM and debug build |
| [results/startup-smoke-final/summary.json](results/startup-smoke-final/summary.json) | One run of `smoke.py` against a real `mo serve`: fifteen failures, records 1 and 15 retained through a snapshot and the final stop, a 20,692-byte failure artifact over three pages, and a mode-0600 file from `mo fetch` |
| [results/failed-checks.txt](results/failed-checks.txt) | The red runs before each green one: missing tables, missing store functions, a rollback not yet classified as a clean refusal, compiler warnings, fixture errors and a missing X-macro definition |
| [results/review-review.json](results/review-review.json) | The code review's receipt, unchanged. Its verdict is the earlier Not-ready snapshot |
| [results/review-reconciliation.json](results/review-reconciliation.json) | How each review finding was resolved or rejected, with file and line references at that commit |

## Development smoke

`smoke.py` is development-only. CI runs its assertions in the `drivers` group through
`ci/check_operator.py` and `ci/check_fetch.py`. Run it against a built `mo` with an output
directory outside the repo, because it writes the full transcript and service stderr there:

```sh
python3 learnability/phase4/failure-capture/smoke.py build/gcc/debug/mo /tmp/mo-failure-smoke
```

## Failures not kept as files

Early smoke attempts omitted the data directory or the protocol-version header, used the wrong
argument representation, or wrote invalid inline Mo; none was an implementation pass. The first
CBMC reach harness for `slice` omitted the text branches and failed; the corrected harness is the
one in `proofs-remediated.txt` and `reach-remediated.txt`. The first TLC jar download did not
match the pinned SHA-256; it was not repinned, and the pinned jar ran instead.

No cross-provider review ran. Offline diagnosis (U5) and evaluation (U6) are not implemented.
