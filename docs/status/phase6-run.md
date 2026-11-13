# Phase 6 run

## Strength of the task queue's intent

The wrong versions are set 1 in `learnability/phase6/strength/` (see its README). Every
measurement is

```sh
mo test services/task-queue/main.mo --strength <folder> --json
```

with each promise on 200 inputs and each service promise on 1,000 sequences.

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
