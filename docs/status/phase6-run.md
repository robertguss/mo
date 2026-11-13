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
