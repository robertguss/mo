# Release command timings

MO-122 records the twelve samples and three small Mo workloads on one standard Amp orb.
This is a baseline, not a speedup claim or a new CI gate.

## Recorded baseline

[results.json](results.json) contains the raw runs from 24 October 2026 at 11:56 UTC,
measured on clean commit
[`6368762`](https://github.com/robertguss/mo/commit/636876209f3f1a08f303faa78bb7789ba1fb31ff).
The later results commit adds only this record and documentation, not measured code.
The standard orb exposed four logical CPUs and 8,147,608 KiB of RAM, with an Intel Xeon
processor reported at 2.60 GHz. The kernel was Linux 6.1.158+, GCC was Debian 12.2.0, and
Python was 3.11.6. All four CPUs were in the process affinity set. The cgroup quota was
not exposed. One-minute load averages were 1.11 before and 1.09 after the serial run,
following the release build. No other build, check, or profiler ran during measurement.

All values below are milliseconds, shown as median [minimum, maximum] over nine runs.
All 350 invocations passed output validation, including 35 warmups and 315 recorded runs.

| Sample | `mo check` | `mo run` | `mo test` |
| --- | --- | --- | --- |
| account.mo | 31.851 [31.367, 33.132] | n/a | 42.056 [41.611, 46.025] |
| expenses.mo | 20.831 [19.609, 22.721] | 34.748 [33.761, 37.264] | 35.423 [34.069, 37.551] |
| find.mo | 19.220 [18.995, 21.140] | 32.709 [31.585, 38.506] | 33.058 [32.265, 34.341] |
| greet.mo | 17.975 [17.387, 26.853] | 28.568 [27.451, 32.444] | 28.214 [27.302, 33.217] |
| inventory.mo | 21.595 [20.489, 28.315] | 39.287 [37.254, 41.544] | 38.988 [37.811, 39.793] |
| job-queue.mo | 35.782 [34.888, 37.087] | n/a | 50.431 [49.187, 51.443] |
| json-query.mo | 22.349 [21.457, 32.938] | 39.708 [37.713, 43.465] | 40.681 [37.855, 53.463] |
| kv.mo | 36.814 [35.481, 48.282] | 37.477 [37.134, 50.372] | 43.321 [40.597, 59.479] |
| notes-tool.mo | 31.018 [30.175, 40.537] | n/a | 33.030 [32.360, 49.678] |
| rate-limit.mo | 33.754 [29.758, 42.351] | n/a | 49.693 [48.389, 61.010] |
| todo.mo | 35.325 [34.325, 37.528] | 35.240 [33.857, 45.962] | 37.618 [36.002, 49.109] |
| wc.mo | 18.852 [18.232, 26.524] | 30.877 [29.637, 33.413] | 30.436 [29.111, 37.068] |

| Benchmark | `mo run` wall time | Median user + system CPU time |
| --- | --- | --- |
| list-building.mo | 142.529 [140.769, 146.947] | 142.234 |
| map-updates.mo | 181.339 [178.221, 209.922] | 180.992 |
| text-processing.mo | 61.109 [58.954, 63.213] | 60.867 |

The CPU times are within 1% of wall time for these workloads, consistent with one CPU
doing the work rather than waiting on external I/O. The short sample commands also pay
startup and compiler costs. These numbers cannot be read as VM-only times or as the
throughput of the underlying C collection functions. Observed run-to-run variation,
including the 209.922 ms map run, stays in the recorded ranges rather than being discarded.

## Reproduce

From the repository root, with GCC, make, Python 3, and a Linux orb:

```sh
python3 bench/performance/measure.py --output /tmp/mo-timings.json
```

Run without other builds, tests, or benchmarks. The script always rebuilds in a separate
directory with this command, before any timing starts:

```sh
make -B -j4 CC=gcc BUILD=build/bench-release DEBUG='-O2 -g -pthread' all
```

The Makefile has no release target. Its `debug` directory name is fixed in the build rules,
so the optimized binary is `build/bench-release/debug/mo`. The overridden flags replace
`-O0` with `-O2`. Debug symbols remain for profiling. Assertions remain enabled, as required
by Mo's implementation contract. No sanitizer, profiler, native CPU tuning, or LTO is enabled.

The script prints milliseconds and writes JSON with all individual wall, user CPU, and
system CPU times in seconds. It records the Git commit, dirty status, source and binary
hashes, build command, compiler, kernel, CPU, memory, CPU affinity, and load averages.
`cpu_max: null` means that the orb does not expose the cgroup CPU quota file.

## Method

Each row has one unrecorded warmup and nine recorded invocations. `--runs N` changes the
recorded count and requires at least five. Jobs run serially in rounds, with the order
reversed on alternate rounds. The page cache is warm, but each invocation is a new process.
There is no CPU pinning, frequency control, cache flush, or subtraction of process overhead.

Wall time uses Python's `perf_counter_ns` immediately around `subprocess.run`. It includes
process startup, source loading, output capture, and all compiler and VM work performed
by that command. A plain check validates lowering but skips bytecode execution unless it
has promise anchors to validate. Validation and summary calculations are outside the timed
interval. CPU times use differences in `getrusage(RUSAGE_CHILDREN)` around each invocation.
Build time and profiling runs are excluded.

Every invocation must exit 0, have empty stderr, and match expected stdout byte for byte.
An error, timeout, or output mismatch aborts without writing a new results file. Sample
outputs, arguments, and stdin come from `tests/golden/`. All twelve have check and test
times. Eight also have run times using the golden workloads named in `RUN_CASES` in the
script. The other four are library modules without `main`, so running them would measure
an E0079 rejection. `todo list` runs in a temporary empty working directory and cannot
read a developer's saved to-do list. These tiny golden inputs are correctness examples,
not large application-throughput workloads. Test times include all examples, generated
promise cases, and tests under the CLI's default settings.

The three benchmark outputs are derived independently of the implementation:

| Program | Work per invocation | Expected stdout |
| --- | --- | --- |
| `list-building.mo` | Append integers 0 through 99,999 to an initially empty list, then sum it. | `100000 4999950000` |
| `map-updates.mo` | Increment 1,000 integer keys over 100,000 updates, then sum the counts and read key 999. | `1000 100000 100` |
| `text-processing.mo` | Generate 10,000 lines, trim, lowercase ASCII, replace commas, split words, and count UTF-8 bytes. | `30000 130000` |

The list sum is 100,000 × 99,999 / 2. Each map key has 100 updates. Each cleaned text line
is `alpha beta λ`, with three words and 13 UTF-8 bytes. The generated input is 170,000 bytes.
Input generation and consumption of the results both occur inside the timed command.

## Profiling separately

For attribution only, build a different binary and aggregate five runs per benchmark:

```sh
make -j4 CC=gcc BUILD=build/bench-profile DEBUG='-O2 -g -pg -pthread' all
mkdir -p build/bench-profile/profiles
for name in list-building map-updates text-processing; do
  rm -f build/bench-profile/profiles/"$name".*
  for i in 1 2 3 4 5; do
    GMON_OUT_PREFIX="$PWD/build/bench-profile/profiles/$name" \
      build/bench-profile/debug/mo run "bench/performance/$name.mo"
  done
  gprof -b build/bench-profile/debug/mo build/bench-profile/profiles/"$name".* \
    > "build/bench-profile/$name.txt"
done
```

The initial profile found `mo_vm_execute` and VM register access prominent in list building.
Map updates also spent samples in `round_hash` and `mo_hash`. These map to
`src/vm/execute.c`, `src/vm/process.c`, and `src/base/hash.c`. Text processing showed VM
work, byte conversion, and list appends, but had only 11 profiler ticks across five runs.
That is too little evidence to name one dominant function. gprof instruments function
calls and changes their costs. Its times are not mixed with the baseline results.

The measurements do not justify another timing threshold. One orb and warm-cache runs do
not establish cross-orb stability. The existing drivers gate remains unchanged and measures
the best of five debug-build `mo check` invocations against 0.1 seconds. It is not the median
release-build statistic recorded here.
