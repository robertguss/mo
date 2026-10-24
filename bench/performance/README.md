# Release command timings

MO-122 records the twelve samples and three small Mo workloads on one standard Amp orb.
This is a baseline, not a speedup claim or a new CI gate.

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

Wall time uses Python's `perf_counter_ns` immediately around `subprocess.run`, including
process startup, source loading, checking, lowering, bytecode generation and verification,
VM execution where applicable, and output capture. Validation and summary calculations are
outside that interval. CPU times use differences in `getrusage(RUSAGE_CHILDREN)` around
each invocation. Build time and profiling runs are excluded.

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
