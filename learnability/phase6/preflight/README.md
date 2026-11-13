# Intent and review door feasibility preflight

The evidence behind [intent and the review door](../../../docs/design/intent.md).
Each prototype is throwaway: it changes no file under `src/` in the repository,
and each `run.sh` rebuilds what it needs from a clean export of 02822786.
Predictions were not written down before these runs; the README of each
prototype states what was expected and what was found. Every functional output
was reproduced from a clean export by a second run, with only timing and
temporary paths differing.

Host for every number below: an exe.dev VM, 8 vCPUs of an AMD EPYC 9554P, 31 GiB,
Linux 6.12.113, gcc 13.3.0, the debug build, which is how the task queue was
deployed (`docs/ops/task-queue.md`). Phase 5's measurements used the same class
of host.

## Service promises on the recovery generator

[service-promise/](service-promise/README.md): `bash learnability/phase6/preflight/service-promise/run.sh`

- The recovery generator's per-step hook was given a result. A failed service
  promise has its own outcome, and the shrinker keeps a shorter sequence only
  when the same promise fails.
- Three task queue guarantees, written as predicates over the state before and
  after a step (two over every step, one over each `submit` and its reply), pass
  on the real service.
- A planted fault in `JobQueue.expire` deletes the newest job when a tick lands
  off the whole minute, which only happens after a restart. Today's recovery
  check passes it. The prototype fails it on the first sequence, shrunk to four
  steps ending in the restart, and two runs print identical output.
- Two more planted faults each break one of the other guarantees.
- Cost at a fixed 200 sequences: 2.6 s without promises, 2.9 s with three, the
  same 28 MB peak.
- Mo Core and Lean refuse a promise function with no subject
  (`src/core/metadata.c:247`, `formal/MoCore/Metadata.lean:63`); the bytecode
  verifier accepts one. A service promise therefore lowers to a plain spec
  function, and Core, Lean and the bytecode stay unchanged.

## Intent item identity

[identity/](identity/README.md): `bash learnability/phase6/preflight/identity/run.sh`

- A standalone program linked against the debug build walks the checked task
  queue and gives each of its 19 items a reach and a digest. The third `jobs`
  example row reads `fail`, `leased_job`, `release` and `put`; the promise "a job
  is attempted at most three times" reads `max_attempts`.
- The formatter prints one declaration alone with no compiler change. A digest
  over that output with comments, blank lines and nested promise and example
  blocks removed ignores a comment inside `release` and a layout-only edit; raw
  bytes do not.
- Nine variants show which digests change with the subject's signature included
  and left out (`identity/out/table.md`). Including it only made a
  meaning-preserving parameter swap wait.

## Timing

[timing/](timing/out/timing.txt): `bash learnability/phase6/preflight/timing/run.sh`

Built from 02822786 with the service-promise patch, the task queue's checks
under a plan fixed by counts (`--seconds 100000`, so no clock cuts them short):

- `mo check`: 0.1 s.
- 200 sequences: 2.8 to 3.7 s without service promises, 3.0 to 3.2 s with three.
- 1,000 sequences: 14.9 to 16.3 s without, 17.0 to 18.0 s with three. Every run
  peaked at 28 MB.
- `queue-tests.mo`, which `main.mo` does not import and a push therefore does
  not carry: 66 s, 64 s of it in the test that fills the queue to 5,000 jobs.
- Today's default 2-second cap stopped the recovery check after 173 of its 200
  sequences and reported a pass.

The limits declared from these numbers are in the design's Limits section.
