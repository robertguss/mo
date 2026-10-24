# MO-119 VM against Lean run

## Commit and method

The run uses main commit
[`267370f50a13e0ce12d103346ae63b6861ba3436`](https://github.com/robertguss/mo/commit/267370f50a13e0ce12d103346ae63b6861ba3436)
with no changes to the generator, comparison driver, C implementation, or Lean model.
The requirement is in [MO-119](https://www.notion.so/403700320c458155b095e900c2e12d75).
Architecture section 11, "Testing strategy", defines the comparison contract.

Environment on 2026-10-24: Linux x86-64, four available CPUs, Python 3.11.6,
GCC 12.2.0 debug build (`-O0 -g`), and Lean 4.34.0 release executable.
Lean was installed through `bash .agents/setup lean` in `/opt/elan`.
`make formal` built the model, checked its proofs, and passed `ci/test_mocore.sh`.

The existing `ci/check_core_differential.py` runs 100 consecutive batches of
10,000 programs, with seeds 0 through 999,999 inclusive. Each seed constructs
a well-typed program through `ci/gen_core.py` with `lowerable=True`.
The C driver checks, compiles, serializes, loads, and executes each program.
Lean checks and evaluates the same program. The comparison checks returned
structural text or the exact stop reason. A stop such as overflow is a compared
outcome, not a test failure.

Each batch also checks `tests/differential/entry-returns-one.core` and the three
text-repeat regression seeds from #125. These repeated checks are not part of
the million unique seeds. Neither are the initial 1,000-seed pilot or the lean
group's default generated run. Budget cutoffs are reported separately and do
not count as agreements.

The generator covers pure expressions, not every Core feature. It excludes
capability operations, globals, recursion, metadata, and spec functions.
The sample comparison and the rest of the lean group exercise additional forms.
This run is evidence for this generator and commit, not a proof of VM equivalence.

## Replay

From the named commit, build the two executables and run the batches. The loop
stops at the first failing batch and saves disagreeing Core programs.

```sh
set -euo pipefail
bash .agents/setup lean
export ELAN_HOME=/opt/elan PATH=/opt/elan/bin:$PATH
make formal
make -j4 CC=gcc build/gcc/debug/tests/test_vm.test
mkdir -p build/mo119
date -u +START=%Y-%m-%dT%H:%M:%SZ
for first in $(seq 0 10000 990000); do
    date -u +BATCH_START=%Y-%m-%dT%H:%M:%SZ
    MO_CORE_DIFF=10000 MO_CORE_DIFF_SEED=$first \
        MO_CORE_DIFF_OUT=build/mo119/mismatches \
        python3 ci/check_core_differential.py \
        build/gcc/debug/tests/test_vm.test || exit
done
date -u +END=%Y-%m-%dT%H:%M:%SZ
python3 ci/check_vm.py build/gcc/debug/tests/test_vm.test
```

The recorded run used a supervised orb service and temporary output paths.
The replay uses the same executable arguments, batch sizes, and seeds.
Batch elapsed times measure generation, file I/O, subprocesses, checking,
compilation, and evaluation, including the repeated regression checks.
They exclude the initial builds. Other local checks ran alongside part of the
run, so these times are not a performance comparison.

## Sample Core

`python3 ci/check_vm.py build/gcc/debug/tests/test_vm.test` passed on the named
commit. It compared 2,495 outcomes, with zero unsupported fixtures and zero
missing primitive, host, or capability dispatch cases. Its additional checks
compared 4,370 numeric/comparison outcomes, 187 structural-text cases, and
12 structure/ownership cases. Decimal arithmetic also passed 2,230 exact
Python rational comparisons.

The twelve sample fixtures under `tests/core/` all returned matching outcomes.
For capability-bearing entries the driver also compared console output, error
output, files, folders, and fake capability state with Lean's report. These
are the committed sample Core fixtures, not a fresh lowering of `examples/`.

| Sample Core | Matching returned value |
| --- | --- |
| `Account.core` | `Ok((Account(owner: "alice", balance: 95), Account(owner: "bob", balance: 25)))` |
| `Expenses.core` | `1` |
| `Find.core` | `0` |
| `Greet.core` | `Unit` |
| `Inventory.core` | `Unit` |
| `JobQueue.core` | `Ok((Queue(jobs: {}, next_id: 1), None))` |
| `JsonQuery.core` | `0` |
| `Kv.core` | `Unit` |
| `NotesTool.core` | `Ok(SearchOutput(hits: [Hit(file: "a.md", line: 1, text: "Needle")], truncated?: false))` |
| `RateLimit.core` | `(Limiter(capacity: 10, per_second: 2, buckets: {"client": Bucket(tokens: 9, updated: 1970-01-01T00:00:00Z)}), Allowed(remaining: 9))` |
| `Todo.core` | `Ok(Unit)` |
| `Wc.core` | `1` |

## Results

All 1,000,000 generated programs agreed. There were 796,644 matching returns,
203,356 matching stops, zero disagreements, zero budget cutoffs, and zero
timeouts. All twelve sample Core fixtures also agreed. No implementation fix
or new minimized regression fixture was needed.

The generated run started at 2026-10-24 11:50:11 UTC and finished at
12:31:08 UTC. Wall time was 2,457 seconds, or 40 minutes 57 seconds.
The batch elapsed times sum to 2,448.3 seconds. The difference includes Python
startup and time between batches. All batches used the same named commit and
unchanged executables.

| Seeds, inclusive | Programs | Matching returns | Matching stops | Batch seconds |
| --- | ---: | ---: | ---: | ---: |
| 0 through 249,999 | 250,000 | 199,472 | 50,528 | 717.5 |
| 250,000 through 499,999 | 250,000 | 199,058 | 50,942 | 588.4 |
| 500,000 through 749,999 | 250,000 | 199,207 | 50,793 | 577.7 |
| 750,000 through 999,999 | 250,000 | 198,907 | 51,093 | 564.7 |
| Total | 1,000,000 | 796,644 | 203,356 | 2,448.3 |

The [batch table](mo119-core-batches.tsv) records each batch's first seed,
program count, compared outcomes, and elapsed seconds. The last seed is the
first seed plus 9,999. Each batch's outcome counts sum to exactly 10,000.
The existing minimized corpus and text-repeat regression checks also passed
in every batch. Progress was sent to the coordinator at each 250,000-program
milestone.

### Record recovery

The supervised service began another pass after the successful command exited.
At 12:31:10 UTC it reopened the output file with `>`, truncating the original
file. The separate log reader had already captured the complete first pass,
including its `START`, all 100 batch results, and `END`. The batch table was
transcribed from that captured output in
[the run thread](https://ampcode.com/threads/T-01a16dbc-8cd4-7578-94a8-3dc5fcec74f0).
It is a recovered table, not the original stdout file. A Python table
audit checked all seed boundaries, per-batch outcome sums, totals, and the
previously recorded 260,000-program checkpoint. The duplicate pass was stopped
and is excluded from every count and time above. Future supervised batch runs
need append-only output or a separate file for each invocation.

### Local checks and review

`bash ci/check.sh gcc` and `bash ci/check.sh lean` passed without skips on the
named commit. The lean group compared the VM with Lean under GCC debug, Clang
debug, and Clang ASan/UBSan builds. Its default 1,000 generated programs are
additional checks of seeds already in this run, not extra unique seeds.
The standalone `clang`, `sanitize`, `drivers`, `fuzz`, `static`, and `cbmc`
groups were not run. No hosted CI was restored.

Self-review checked the batch arithmetic, sample coverage, comparison failure
paths, generator exclusions, and the absence of implementation changes.
No new behavior or test was added, so no mutation test was needed.
Independent review was not run because this task explicitly required one
agent with no other threads. The coordinator decides whether to merge.
