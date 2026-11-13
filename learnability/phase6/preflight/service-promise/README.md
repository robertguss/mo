# Service promises: feasibility prototype

A throwaway prototype, built on commit 02822786, that tests two planning decisions:

- A service promise is a predicate that `mo test` checks against the sequences the recovery
  check already generates. It never drives a `Simulation`.
- By default a service promise is checked at every step, over the state before the step, the
  step with its reply, and the state after it. A crash and its restart count as one step, and
  state reaches the predicate through the saved codec.

Verdict: both hold up. The recovery check's per-step hook can fail a sequence, the shrinker keeps
a shorter sequence only when the same predicate fails, state decodes cleanly through the saved
codec, and a fault that the existing recovery check cannot see is found and shrunk to a
four-step sequence that ends in the restart. On the task queue, three predicates cost about 13%
more time per sequence. Nothing in this prototype is a blocker.

## Files

| File | What it is |
| --- | --- |
| `prototype.patch` | `git diff 02822786 -- src/ tests/`. The `tests/` part only updates two unit tests to the new hook field and `mo_recovery_shrink` signature, so `make test` still builds. |
| `promised/main.mo`, `promised/job-queue.mo` | The task queue with three guarantees added at the end of `main.mo`. `job-queue.mo` is unchanged. |
| `faulty/main.mo`, `faulty/job-queue.mo` | The same, plus one planted fault in `JobQueue.expire`. |
| `run.sh` | Exports 02822786 twice with `git archive`, applies the patch to one copy, builds both with `make CC=gcc`, and writes every output below to `out/`. |
| `out/` | Raw outputs from the last `run.sh` run. |

## What the prototype changes in `src/`

- `src/tools/recovery.h`: `struct mo_recovery_hooks` gains a third callback,
  `uint32_t (*check)(observation, context)`. It returns `MO_NO_INDEX` when the step kept every
  promise, or an id naming the promise that failed. The observation gains `before` and `after`,
  the state's saved encodings. There is a new outcome, `MO_RECOVERY_PROMISE`, and the result gains
  `promise`, the id that failed. `mo_recovery_shrink` now takes that id and the hooks, and a new
  accessor, `mo_recovery_process`, lets the hook call Mo functions in the checker's process.
- `src/tools/recovery.c`, `run_step`: when a check hook is set, the state is encoded before each
  step except a crash. A restart's "before" is the encoding already taken at its crash. After the
  step, and after a restart's startup ticks have run, the state is encoded again and the hook is
  called. A failure ends the sequence with `PROMISE` at that step, and both encodings move into
  the result. The existing `observe` hook and the equality check are unchanged.
- `src/tools/recovery.c`, shrinker: a candidate is kept only when its replay ends in the same
  outcome **and** the same promise id. The assertion that the failing step is a restart now holds
  only for MISMATCH and REFUSED.
- `src/tools/tests.c`: `find_guarantees` collects the service module's functions named
  `<Module>.guarantee_*`, and `check_guarantees` is the hook. It decodes `before` and `after` with
  `mo_value_decode` at the service's `state_type`, then calls each predicate with `mo_call`.
- `src/tools/render.c`: the failing step is marked `broke <function>`, and the state before and
  after that step are printed. JSON gets a `"broke"` field, `"before"` and `"after"`.

## The shape used for the guarantees (and what was awkward about it)

The guarantees are ordinary Mo functions in the service's module, found by name. The parameter
list decides when a guarantee is checked:

- `guarantee_X(before: State, after: State) : Bool` is checked after every step: a tool call
  whatever its reply, a clock advance with all its firings, and a crash with its restart and the
  startup ticks.
- `guarantee_X(before: State, <tool arguments>, reply: <tool's complete result type>, after: State) : Bool`
  is checked after each call to the tool whose argument and result types match, when that call
  replied or was rejected.

```mo
fn guarantee_no_job_lost(before: Queue, after: Queue) : Bool
  kept = JobQueue.jobs(after).map(fn(job) job.id end)
  JobQueue.jobs(before).all?(fn(job) kept.contains?(job.id) end)
end

fn guarantee_finished_jobs_stay(before: Queue, after: Queue) : Bool
  now = JobQueue.jobs(after)
  JobQueue.jobs(before).all?(fn(job)
    not (job.state == JobQueue.Done or job.state == JobQueue.Dead) or now.contains?(job)
  end)
end

fn guarantee_submit_leaves_ready(
  _before: Queue,
  payload: Text,
  reply: Result((Queue, Int), SubmitError),
  after: Queue
) : Bool
  case reply
    Ok((_, id)):
      JobQueue.jobs(after).contains?(Job(id: id, payload: payload, attempts: 0, state: JobQueue.Ready))
    Error(_): true
  end
end
```

What was awkward, for whoever designs the source syntax:

1. **Matching a predicate to a tool by its types is ambiguous.** `heartbeat`, `acknowledge` and
   `fail` all take `(Int, Int)` and return `Result((Queue, Unit), QueueError)`, so a predicate
   written for `fail` would be attached to `heartbeat`, the first match. A tool predicate has to
   name its tool.
2. **The reply type is the handler's complete result,** `Result((Queue, Int), SubmitError)`. It
   carries the new state again, beside `after`, and the author has to spell the whole type.
   Passing only the reply part (what `mo_recovery_reply` already extracts) and naming the tool's
   arguments would read better.
3. **An every-step predicate cannot tell which step it is looking at.** It sees no step value, no
   step time and no advance duration, so it cannot say "only across a restart" or "only when no
   time passed". Giving it one needs a per-service `Step` choice type (a call with its tool and
   arguments, an advance with its duration, a restart) that the tools construct. That type does
   not exist today, and building it is the first piece of real new machinery the feature would
   need.
4. **The state type was opaque from the predicate's module.** `Main` sees `Queue` without its
   fields (`expose Queue` in `job-queue.mo`), so the guarantees go through `JobQueue.jobs`. That
   worked here, but a service whose state is opaque and has no accessor could not be stated from
   the service module.
5. **One `Bool` per function means one condition per promise.** Real promises compile to an `Int`
   that names the first false line. The shrinker compares whatever id the hook returns, so
   per-line ids are a drop-in change.
6. **A predicate that stops or overflows aborts `mo test`.** The prototype asserts that
   `mo_call` returned. The real feature needs an outcome for this, as ordinary promises have.
7. **Steps where the state cannot be read are skipped without a word.** A call to a crashed or
   stopped service is one. The real feature should count these, or say they are exempt.

## Results

Every command below is run by `run.sh` from a temporary directory holding `promised/`,
`faulty/`, `original/` (a copy of `services/task-queue/`) and the two mutants. The output is in
`out/`, and each run's exit status is appended as `exit N`.

### 1. The existing recovery check passes on the faulty copy

The fault, in `faulty/job-queue.mo`, `JobQueue.expire`:

```mo
  return after if (now - t0).in_milliseconds % 60000 == 0
  after.jobs = after.jobs.remove(after.next_id - 1)
```

Until the first crash, every timer firing lands on a whole minute after the simulation's epoch.
After a restart, the timers start from the restart's moment, so the startup tick and every later
tick can land off the minute, and each one deletes the newest job.

Command, with the unpatched binary: `mo test faulty/main.mo --only Main.service.recovery --calls 200 --seconds 100000`

Output (`out/baseline-faulty.txt`):

```
passed Main.service.recovery  faulty/main.mo:15:1
1 passed, 0 failed, 0 stopped
exit 0
```

It passes because the existing check compares the state before the crash with the recovered state
**before** the startup ticks run (`src/tools/recovery.c:668`, then the drain at `:696` in
02822786). The job vanishes in the startup tick that follows. At every later crash the state
already lacks the job, so recovery reproduces it exactly and the comparison is equal again.

### 2. The prototype fails it with a shrunk sequence that contains the restart

Same command, patched binary (`out/prototype-faulty-1.txt`):

```
failed Main.service.recovery  faulty/main.mo:15:1
  sequences: 1
  1. advance 0.000001s  0 timer firings
  2. submit("")  replied 1, committed
  3. crash
  4. restart  recovered the same state  broke Main.guarantee_no_job_lost
  state before step 3: Queue(jobs: {1: Job(id: 1, payload: "", attempts: 0, state: Ready)}, next_id: 2)
  state after step 4: Queue(jobs: {}, next_id: 2)
0 passed, 1 failed, 0 stopped
exit 1
```

The shrunk sequence is minimal for this fault: the 1 µs advance moves the clock off the minute
grid, the submit makes a job, and the restart's startup tick deletes it. The restart line shows
both checks: recovery reproduced the state exactly, and the guarantee still broke.

### 3. Determinism

A second run of the same command is written to `out/prototype-faulty-2.txt`. `cmp` finds the two
outputs identical (`out/determinism.txt` says `identical`). The search is seeded from the check's
name (`recovery.c`, `mo_recovery_new`), and the predicate hook adds no randomness.

### 4. Each guarantee can fail, and does when broken

These are sensitivity checks on two mutants of `promised/`, made by `sed` in `run.sh`. The diffs
are in `out/mutant-*.diff`.

- `mutant-finished`: `leased_job` accepts a Done job, so `heartbeat`, `acknowledge` or `fail` can
  change one. Result in `out/prototype-mutant-finished.txt`: found in sequence 21, shrunk to six
  steps, ending in `6. heartbeat(1, 0)  replied Unit, committed  broke Main.guarantee_finished_jobs_stay`,
  with the Done job before and the Leased job after. This also shows that generated sequences
  reach Done jobs.
- `mutant-submit`: submit creates the job with `attempts: 1`. Result in
  `out/prototype-mutant-submit.txt`: `1. submit("")  replied 1, committed  broke Main.guarantee_submit_leaves_ready`.
  This guarantee is the one that reads the reply.
- `promised/` itself passes (`out/prototype-promised.txt`), and so does the unmodified service
  under both binaries (`out/baseline-original.txt`, `out/prototype-original.txt`).
- The recovery unit tests pass on the patched tree (`out/unit-tests.txt`:
  `recovery faults: all checks passed`, `recovery: all checks passed`).

The `mutant-finished` sequence keeps two steps it does not strictly need (`3. fail(1, 1)` and
`4. lease()`). `acknowledge(1, 2)` carries the attempt as a literal 2, so deleting those steps
alone makes the acknowledge refuse. The existing shrinker has the same limit; it is not new.

### 5. Cost

Command for each row: `/usr/bin/time -v mo test <program>/main.mo --only Main.service.recovery --calls 200 --seconds 100000`.
Debug build (`-O0`), 200 sequences, three runs each (`out/cost.txt`):

| Binary and program | Wall (runs 1, 2, 3) | Median wall | Per sequence | User CPU (median) | Max RSS |
| --- | --- | --- | --- | --- | --- |
| Baseline, original service | 2.58, 2.71, 2.55 s | 2.58 s | 12.9 ms | 2.56 s | 27.8 MB |
| Prototype, original service (no guarantees) | 3.02, 2.61, 2.51 s | 2.61 s | 13.1 ms | 2.59 s | 27.8 MB |
| Prototype, three guarantees | 3.17, 2.92, 2.87 s | 2.92 s | 14.6 ms | 2.90 s | 28.1 MB |

With no guarantees the hook is absent and nothing is encoded, so the cost is within noise. Three
guarantees add about 13% wall time and 0.3 MB. The prototype is naive: it encodes the state twice
per step and decodes it twice per step, although one step's "after" is the next step's "before".
Reusing that encoding would remove about half of the codec work.

## Finding: do Mo Core and the bytecode verifier admit a promise with no subject?

**The bytecode loader and verifier do; Mo Core's source metadata does not, in C or in Lean.**

Bytecode admits it:

- `src/bytecode/validate.c:388-394`: a PROMISE needs at least one parameter and an `Int` result.
  Nothing more.
- `src/bytecode/validate.c:650-659`: the subject is checked only when it is not `NO_INDEX`.
  Condition lines are allowed on any PROMISE (`:642`).
- `src/bytecode/verify.c:631-633`: the check that "promise parameters differ from subject" is
  skipped when the subject is `NO_INDEX`.
- `src/bytecode/bytecode.h:425-427` says so: "Source-less Core has no links/lines". PROMISE
  functions with subject `NO_INDEX` already load today.

Mo Core refuses it whenever source metadata is present:

- `src/core/metadata.c:247-248`: `linked = kind == PROMISE || kind == ANCHOR` and
  `if (f->subject.some != linked) return false`. A PROMISE without a subject is invalid metadata.
- `formal/MoCore/Metadata.lean:63-66`: `| .promise, none | .anchor, none => false`, the same rule
  in Lean. `formal/TEXT.md:299-304` documents it.
- A promise's kind signature, by contrast, is fine for any parameter list (`src/core/check.c:1734`,
  `formal/MoCore/Syntax.lean:664-670`: parameters non-empty and an `Int` result).

So compiling a service promise to a subject-less PROMISE **would require changing the Core
metadata rule in both `src/core/metadata.c` and `formal/MoCore/Metadata.lean`**, and with it the
Lean proofs that rely on it. It needs no bytecode format change and no verifier change. There
are two ways to avoid touching Lean:

- **Emit the predicate as an ordinary FUNCTION**, as this prototype does. Core and the verifier
  accept it unchanged. The cost is that it is not spec code (`spec_code` false), so it carries no
  `text` or condition `lines` in Core metadata (`metadata.c:239-242`, `Metadata.lean:59-61`). The
  tools would then have to keep the promise's sentence and lines in their own table, keyed by the
  function, and so would `mo test`'s reporting.
- **Give a tool promise the tool's handler as its subject.** A PROMISE's parameters must be its
  subject's parameters plus the result (`verify.c:634-639`). For a tool handler that is
  `(state, now, arguments..., result)`, and the new state is inside an update tool's result. A
  timer handler gives `(state, now, new state)`. Those are exactly a per-step tool or timer
  promise, and today's Core, verifier and Lean admit them unchanged. What is new is only that the
  tools check them against reachable states rather than generated ones. This does not cover an
  every-step promise across a crash and restart, which has no handler to be its subject. That one
  needs the ordinary-FUNCTION route or the Core rule change.

Lean evaluates no `Simulation` operation: `formal/MoCore/Builtins.lean:1031-1034` expects
`stopped (.outside "Simulation is not modeled by mocore")`. A predicate over plain values (state,
arguments, reply) is ordinary Core, so the VM-versus-Lean comparison still covers it. The
predicates in this prototype use only lists, maps, records and closures.

## Finding: the per-step form or the whole-history form

The whole-history form is a promise over the list of all steps, each step being
`(before, step, reply, after)`, or over the list of states.

| Guarantee | Per-step form | Whole-history form | Simpler |
| --- | --- | --- | --- |
| No job is ever lost, across any step including a crash and restart | `before`'s ids are all in `after` (two lines, above). | For every state *i* and every later state *j*, `states[i]`'s ids are in `states[j]`: two nested loops over the history, or `history.all?` of the per-step body. | Per-step. Inclusion is transitive, so the per-step form implies the history form. |
| A Done or Dead job never changes again | Each Done or Dead job in `before` is in `after`, unchanged. | Each Done or Dead job in any state equals that job in every later state: nested loops again. | Per-step, for the same transitivity reason. |
| A submit that replied with an id leaves that job Ready | On a `submit` that replied `Ok((_, id))`, `after` holds `Job(id, payload, 0, Ready)`. | `history.all?(fn(s) s.step is a submit and s.reply is Ok((_, id)) implies s.after holds the job end)`. Either the step value has to be a per-service choice type the author matches on, or this is the per-step body wrapped in `all?`. | Per-step. The history form adds matching on a step type that does not exist yet. |

All three read more simply per step, and none needs history. The whole-history form earns its
place only for guarantees that relate steps that are not adjacent, and that the state does not
record. Two examples: "an id that `submit` returned is never returned again", for a service whose
state does not keep `next_id`; and "a lease that ran out is Ready again within one timer
interval", which relates the time of one step to a later one. Both are plausible later additions,
not defaults. Per-step as the default is supported.

## What was blocked or left unverified

- Nothing was blocked. Every tool needed was present.
- The full `bash ci/check.sh gcc` was not run, because this is a throwaway prototype. Only the two
  recovery unit tests ran on the patched tree. Both gcc and clang build the patched `mo`.
- Cost was measured on the debug build only, on one service, at one size (200 sequences).
- The shape of the guarantees is a naming convention (`guarantee_*`) read by the tools. It is not
  a proposal for the source syntax.
