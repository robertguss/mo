# Phase 6 run

## Strength of the task queue's intent

The wrong versions are set 1 in `learnability/phase6/strength/` (see its README). Every
measurement is

```sh
mo test services/task-queue/main.mo --strength <folder> --seconds 10 --json
```

with each promise on 200 inputs and each service promise on 1,000 sequences. The records
before `results/4-after.json` ran with the default `--seconds 2`; why the rest use 10 is below.

### First measurement, working half: prediction

Written on 2026-11-13 from the program and the working half's mistakes, before the first run.

- The original passes its own 23 intent items, and no version is `invalid` or
  `changes_intent`, since the writer checked both.
- Killed: `w04` (an expiry that keeps a lease ending exactly now fails `expire`'s first example),
  `w06` (a 60-minute lease breaks the service promise "a lease ends at most 30 minutes from now")
  and `w10` (an expiry that marks every expired lease Dead fails `expire`'s first example).
- Survived: the other nine. Six change only `Main` handlers that no service promise judges
  (`w01` fail acknowledges, `w02` limit, `w07` stats, `w09` NotLeased, `w11`, `w12` list), and
  three change expiry in ways the two `expire` examples and the service promises do not reach
  (`w03`, `w05`, `w08`).
- Rate: 3 of 12, 25%. The set is narrow: the writer could not replace `submit`, `lease`,
  `acknowledge`, `fail`, `leased_job`, `release`, `put` or `jobs`, because the service promises'
  anchors and the examples call them, so every such version changed intent.

### First measurement, working half: result

`results/1-before.json`: 3 of 12 killed, 25%, not partial. The prediction held exactly: `w04`
and `w10` killed by an example, `w06` by a promise, the other nine survived.

### Strengthening

One existing item was reshaped, as approved: the third `jobs` example row moved, unchanged, to
`fail`'s examples. Everything else was added, written from each tool's `##` description and
the modules' own documentation:

- `JobQueue.expire`: a promise that every ended lease, and only those, goes back to Ready or to
  Dead on its third attempt, with every job kept; and rows for two leases ending at once and a
  third attempt expiring.
- The service: a tool promise on `submit` (refusals only for an oversized payload or a full
  queue), `lease` (the oldest ready job, for 30 minutes, one more attempt), `heartbeat`,
  `acknowledge` and `fail` (only your current lease, and what each leaves behind), and `stats`
  (each count matches the jobs in that state).
- `Main.list`: a function promise and three rows, since a tool promise cannot read `list`'s
  `after` argument (E0107) and renaming it would change the service's wire API.
- `Main.submit`: rows at the payload limit and for a full queue whose 5,000 jobs include a Done
  one. `Main.expire`, the one handler with no intent, got the boundary rows `JobQueue.expire` has.

The new items read promise-only helpers (`job_of`, `current?`, `matching`, `wanted?`,
`expired`), never the helpers the handlers use, so a wrong version of a handler's helper is
judged and not reported as changing intent. They also read the existing anchors `one_job`,
`leased_job` and `done_job`, which the service promises already read, so no new item reads
`JobQueue.fail`. The working half's `counted` file lists all 12 versions, fixing the counted
set at the first measurement.

### After strengthening, working half

| Record | Killed | Note |
| --- | --- | --- |
| `results/2-after-constant.json` | 11 of 12, 91.7% | The full queue was a constant, which every start of the program computed: 1.6 s more on a debug build, `mo serve` included. Moved into its example row. |
| `results/3-after-partial.json` | 10 of 11, partial | Exit 1 with E1432: under `--strength` each example has the `--seconds` deadline, 2 s by default, and the full-queue row takes about 1.6 s on a debug build, so `w02` did not finish. |
| `results/4-after.json` | 11 of 12, 91.7% | The same intent as the partial run, measured with `--seconds 10`. Every later measurement uses `--seconds 10`. |

`w11` survives: the `Done` filter of `list` also showing Dead jobs. The `list` promise is weak
in practice. Its generated `limit` is rarely between 1 and 100, and its generated queues are
built from `JobQueue`'s exposed operations, which rarely reach a Dead job. Killing `w11` takes
a row whose queue holds a Dead job, which `Main` can build only through `JobQueue.expire` or
`JobQueue.fail`. That would make fixes to the function it calls wait for review, so it is left
for Robert to decide.

### Held-back half: prediction

Written on 2026-11-13 while the first held-back measurement ran, before any of its output was
read. This session has never seen the held-back files or their mistakes: the manifest holds only
their names and hashes, which still match.

- The original passes, and the run is not partial.
- Killed: 7 to 10 of 12. The intent was strengthened against the working half, so the held-back
  rate should fall below its 91.7%. The weak spots are the same ones `w11` showed: `list` on a
  queue holding Dead jobs, and anything only `Main` sees that no row reaches.
- A version that replaces a helper a new item reads is `changes_intent` and stays in the
  denominator as not killed, since the `counted` file lists all 12.

### Held-back half: result

`results/5-held-back.json`: 11 of 12 killed, 91.7%, not partial, measured at `21c06316`, after
the review's fixes, which leave the working half's record unchanged byte for byte. Eight were
killed by a promise, two by an example and one by both; `w20` survived. None was invalid or
changed intent.

The prediction was wrong: the rate did not fall below the working half's. The likeliest reading
is that the new tool promises judge the service as a whole rather than the working half's
particular mistakes, so they catch mistakes they were not written against. This session still
has not read `w20` or any held-back mistake.

The held-back set is now spent.

Robert's answer, 2026-11-14: accepted. Two limits stand beside the acceptance:

- Twelve versions is a small sample: 11 of 12 has a 95% Wilson interval of about 65% to 98%.
- The set is narrow. The digest rule keeps a wrong version from replacing anything an item
  reads, so neither half could touch `submit`, `lease`, `acknowledge`, `fail` or the anchors.
  The rate says the intent is strong against mistakes in `heartbeat`, `expire` and the `Main`
  handlers, and nothing about the operations its own items read.

`w11` stays a known gap. A row that kills it would read `JobQueue.expire` or `JobQueue.fail` and
make every fix to them an intent change. The better fix is an input generator that builds
queues reaching rarer states, such as Dead jobs, which would help every promise over a hidden
type. That is follow-on work, not part of this measurement.

## The review door's proof on the task queue

Run on 2026-11-15 on the debug build of commit `42442b1f`, the last commit with code, with
`ci/check_door.py`:

```sh
python3 ci/check_door.py build/gcc/debug/mo learnability/phase6/door/planted --all \
  --bypass build/gcc/debug/mo-door-bypass
python3 ci/check_door.py build/gcc/debug/mo learnability/phase6/door/planted --all \
  --wrong /home/exedev/code/mo-strength-held-back/set-1 \
  learnability/phase6/strength/results/5-held-back.json --bypass build/gcc/debug/mo-door-bypass
```

The plan is fixed, so there is no seed (`seed=none`). Each run serves the task queue, approves all
41 of its items through `mo review`, and pushes every case under both settings from a copy of
that state, through real `mo serve`, `mo push` and `mo review`. The cases are the 20 frozen
planted changes in `learnability/phase6/door/planted/` and the 12 wrong versions of one half of
the strength set, 11 counted as killed and one as survived.

| Cases | `intent` | `everything` |
| --- | --- | --- |
| 11 fixes | 11 activated | 11 held |
| 9 intent changes | 9 held | 9 held |
| 11 killed wrong versions, working half | 11 refused, E1433 | 11 refused, E1433 |
| `w11`, survived, working half | activated | held |
| 11 killed wrong versions, held-back half | 11 refused, E1433 | 11 refused, E1433 |
| `w20`, survived, held-back half | refused, E1433 | refused, E1433 |

Each run answered one held push with `mo review --yes ID` and one with `--no ID`, and the old
program answered every call made during every push. For every case that passed its checks, the
door's classification equalled the declared kind. The working-half run took 492 seconds, and
the held-back run 510.

The control: the same cases against `mo-door-bypass`, a build whose push decision activates
every candidate that passes its checks. It failed as required, with exactly one mismatch for
each push the door held: 30 in the working-half run and 29 in the held-back run. Each run passes
only if that control fails.

Two failures from earlier full runs that day are kept:

- **`p19` was declared with the wrong kind.** It renames `submit`'s unused parameter. The door
  held nothing, and the first run failed on it. The brief the door's session gave the
  planted-change session said an item's content includes its subject's signature. The approved
  identity leaves the
  subject's signature out, as Robert settled on 2026-11-13, so `p19` changes no approved item.
  The error was in that brief, not in the door. The manifest keeps the writer's `kind` and adds
  `corrected_kind` with the reason.
- **`w20` was refused although the strength record counts it as survived.** The strength measure
  runs promises and examples. The door also runs the program's tests, and `JobQueue.test.4`
  fails on `w20`. The driver had expected every survivor to behave as a fix. It now accepts a
  refusal with E1433 or the fix outcome for a survivor. Every killed version must still be
  refused.

The drivers group runs a short version: two planted changes of each kind, two killed wrong
versions and one survivor, under both settings, with the control.

## Release evidence for v0.3

Run on 2026-11-18 on `main` at `2b6335c3e8c14c8c16c489adae830e71ab4d1c32`, in a worktree of its
own. `git diff --stat cb59735d HEAD -- src ci cbmc fuzz tests services Makefile` printed nothing:
the one commit after `cb59735d`, which set the version, is a plan document, so this is the 0.3.0
code tree. `bash ci/check.sh gcc` ended with `check.sh finished: pass`, and
`build/gcc/debug/mo --version` printed `mo 0.3.0`. The `gcc` group does not build the two control
executables, so they were built with
`make -j8 build/gcc/debug/mo-door-bypass build/gcc/debug/mo-checks-fail CC=gcc`.

Two jobs then ran side by side on the development VM (8 vCPUs, gcc 13.3.0, clang 18.1.3,
CBMC 5.95.1), so every duration below was measured under the other job's load. The first ran
these in order, and each exited 0:

| Check | Command | Result | Seconds |
| --- | --- | --- | --- |
| Door proof, working half | `python3 ci/check_door.py build/gcc/debug/mo learnability/phase6/door/planted --all --bypass build/gcc/debug/mo-door-bypass` | pass; the control failed with its 30 expected mismatches | 954 |
| Door proof, held-back half | the same with `--wrong /home/exedev/code/mo-strength-held-back/set-1 learnability/phase6/strength/results/5-held-back.json` before `--bypass` | pass; the control failed with its 29 expected mismatches | 995 |
| Simulation | `python3 ci/check_simulation.py build/gcc/debug/mo` | pass | 25 |
| Task queue | `python3 ci/check_task_queue.py build/gcc/debug/mo` | pass | 88 |
| Review | `python3 ci/check_review.py build/gcc/debug/mo build/gcc/debug/mo-checks-fail` | pass | 127 |
| Kill loop | `python3 ci/check_kill_loop.py build/gcc/debug/mo --kills 500 --seed 2026111701` | pass; 500 kills, 20 of them during a commit | 3,583 |
| Models | `python3 ci/check_models.py` | pass; 18 configurations | 24 |
| CBMC | `bash ci/check.sh cbmc` | pass; 75 proofs and 75 reach checks, none skipped | 2,141 |

The second job was `make --no-print-directory fuzz-smoke FUZZ_SECONDS=300`: 43 targets in 44
runs of 300 seconds each, since `fuzz_bytecode_io` runs with a small and a large input limit. It
exited 0 after 13,468 seconds, with 131,154,382 inputs executed in all and no crash, timeout or
leak artifact. Four targets are slow and ran few inputs: `fuzz_protocol_server` 116,
`fuzz_mcp_rpc` 1,171, `fuzz_strength` 1,756 and `fuzz_fetch` 1,889. For those, 300 seconds is a
thin run.

**The door.** Both runs have `seed=none`, 32 cases, 41 approved items and 64 pushes, with one held
push answered yes and one answered no. Every outcome equals the table in the section above:
11 fixes activated under `intent` and held under `everything`, 9 intent changes held under both,
and 11 killed wrong versions refused under both, in each half. `w11` activated under `intent` and
was held under `everything`; `w20` was refused under both. The door's part took 463 seconds in
the working-half run and 482 in the held-back run; the bypassed build's part took 474 and 497.

**The kill loop.** Seed 2026111701, 15 data folders, 24,258 calls: 23,931 answered, 65 refused
and 327 unknown (a lost reply or an `unknown` effect), each of which reconciled as applied in
full or not at all. Of the 500 kills, 426 came during calls, 73 during a push and one after a
migration. Twenty left a transaction half written: `mo history` reported `recovery required` for
those 20 folders and read the other 480, then read all 20 after their restart. Of 136 pushes, 44
activated, and 19 invalid ones were refused: E1426 16 times, and E1423, E1417 and E1416 once
each. The driver prints `internal_stage_coverage=unobserved`: it does not see which internal
stage each kill landed in.

**The models.** The pinned `tla2tools.jar` (v1.7.4, sha256 `936a2620…0e88`) checked `Capture`,
`Export`, `ExportProgress`, `Swap` and `SwapDoor`, which passed, and 13 negative configurations,
each of which produced the counterexample it must. For the door those are `SwapBadVersion`
(`NoUnreviewedIntent`) and `SwapBadReview` (`YesNamesCandidate`).

**The nightly.** The latest run,
[44131355230](https://github.com/robertguss/mo/actions/runs/44131355230), checked `cb59735d` on
2026-11-17 and passed. The two before it, on 2026-11-15 and 2026-11-16, failed on the
live-upgrade check's timer assertion under the sanitizer build, which #355 fixed. No nightly has
run on `2b6335c3` yet; its code is the same.

No check failed in this run. Failures from before it stay on record: `p19` and `w20` in the
section above, the two red nightlies, and two CBMC failures repaired on 2026-11-17 in #356. One was a verifier proof whose harness never reached the simulation instruction, so its
reach check failed; the other was an array allocation proof that no longer finished.

### Verdict

This evidence shows the door's mechanism: it holds what changes approved intent, lets a fix
through under `intent`, refuses what the intent kills, and a build without the decision fails the
same run. It does not show how Robert judges real changes, because he has judged none through the
door. His verdict on the setting is "not yet judged", and the setting stays `everything`.

Open work:

- Real use through the door on the live host: agents changing the task queue with `mo push`.
- Robert's first approvals on the live host.
- The four access checks marked "not yet run" in [the live service runbook](../ops/live-service.md).
- Removing the development VM's exe.dev key from Robert's account.
