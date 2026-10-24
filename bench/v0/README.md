# Agent benchmark v0

Phase 2's baseline of how well fresh agent sessions write Mo (MO-62): twenty tasks of the four
kinds `learnability/protocol.md` names, five of each, with four held out. Each task's text,
starting files and hidden tests are frozen here before any session sees them, as
`bench/tools/` freezes the three tools' tasks. A round is run, graded and reported as the
protocol says, with one session per task per family (D255), and only once Robert has said to
start it; its `freeze.md` (the `mo` commit, the prompt, the models, the seeds) is written then.

| Task | Kind | Held out | Hidden tests |
| --- | --- | --- | --- |
| [01-rle](01-rle/task.md) | write a program | | 10 tests |
| [02-col-sum](02-col-sum/task.md) | write a program | | 15 runs |
| [03-brackets](03-brackets/task.md) | write a program | | 15 runs |
| [04-line-counts](04-line-counts/task.md) | write a program | | 18 runs |
| [05-ranges](05-ranges/task.md) | write a program | yes | 20 runs |
| [06-temperature](06-temperature/task.md) | fix compile errors | | 6 tests, 3 runs |
| [07-grades](07-grades/task.md) | fix compile errors | | 6 tests, 3 runs |
| [08-playlist](08-playlist/task.md) | fix compile errors | | 7 tests, 3 runs |
| [09-rpn](09-rpn/task.md) | fix compile errors | | 8 tests, 3 runs |
| [10-restock](10-restock/task.md) | fix compile errors | yes | 8 tests, 5 runs |
| [11-calendar](11-calendar/task.md) | make a failing test pass | | 10 tests |
| [12-search](12-search/task.md) | make a failing test pass | | 9 tests |
| [13-titles](13-titles/task.md) | make a failing test pass | | 11 tests |
| [14-wallet](14-wallet/task.md) | make a failing test pass | | 12 tests |
| [15-parking](15-parking/task.md) | make a failing test pass | yes | 10 tests |
| [16-todo](16-todo/task.md) | extend a program | | 16 runs |
| [17-stats](17-stats/task.md) | extend a program | | 11 tests |
| [18-store](18-store/task.md) | extend a program | | 12 runs |
| [19-cart](19-cart/task.md) | extend a program | | 15 tests |
| [20-timesheet](20-timesheet/task.md) | extend a program | yes | 18 runs |

A *test* is a `test` in a hidden Mo module, run with `mo test`; a *run* is one `mo run` of the
program, whose standard output and exit status (and, where the test says, standard error and
the files left behind) must match exactly.

## What a session gets

Its prompt holds the task's `task.md` where the round's prompt marks the task. Its empty work
folder is given a copy of the task's `start/` folder first, when the task has one: the program
to fix or extend, and its tests. It never sees `hidden/` or this repository. The task text names
the file of the program's main module, and, for a module other code calls, what it exposes.

## The kinds

- **Write a program** (01 to 05): no start folder. The text says what to write; the hidden
  tests run the program or call its exposed functions.
- **Fix compile errors** (06 to 10): `start/` holds a program, written as someone new to Mo
  might write it, on which `mo check` reports errors, and its test module. The hidden tests are
  that test module, copied back over the session's, and runs of the program, so a fix that
  changes what the program does fails.
- **Make a failing test pass** (11 to 15): `start/` holds a module with one bug and its test
  module, in which exactly one test fails, the one the text names (`failing_test` in
  `task.json`). The hidden tests are that test module, copied back
  over the session's, so editing or deleting a test doesn't help, and a second module,
  `hidden-tests.mo`, that tests the same behaviour on inputs the visible tests don't name, so
  special-casing the visible test's inputs doesn't help either.
- **Extend a program** (16 to 20): `start/` holds a working program, and its test module where
  it has one. The hidden tests cover the new behaviour and the old.

## Each task's folder

| Path | What it is |
| --- | --- |
| `task.md` | the text a session reads |
| `task.json` | the kind, whether it is held out, the main module's file, and the failing test's name |
| `start/` | the files the session's folder starts with (every kind but write a program) |
| `hidden/reference/` | the reference solution: the session's folder as the task's author would leave it |
| `hidden/broken/` | a deliberately broken solution, which must fail a hidden test |
| `hidden/spec/` | the hidden Mo test modules, copied over the session's files before they run |
| `hidden/runs/` | the hidden runs, one folder each; `grade.py` describes the files in one |
| `SHA256SUMS` | the hash of every other file in the folder |

Each expected output was written by hand from the task text, before the reference was run on
it; where the two disagreed, the text decided.

## Grading

```sh
python3 bench/v0/grade.py 05-ranges SESSION/work --mo FROZEN_MO --grammar MO_GRAMMAR
python3 bench/v0/grade.py 05-ranges --reference --mo build/gcc/debug/mo
python3 bench/v0/grade.py --check --mo build/gcc/debug/mo      # what CI runs
```

The verdict is protocol section 3's: the first step that fails, of 1 parses (the executable
grammar, `build/<compiler>/debug/mo-grammar` from `make grammar`, accepts every `.mo` file),
2 checks (`mo check` on the main module reports no diagnostic), and 3 does the task (every hidden
test and run passes). A folder whose only diagnostics are E1103 or E1150 may be a toolchain gap,
and its hidden tests still run. It is one only when each of those diagnostics is on a form the
reference requires, which the round checks by reading it: this build also reports E1103 for some
mistakes, such as calling a function that doesn't exist through its module. The model graders and Robert's check (protocol sections 4 and 5)
come after this, on what the sessions wrote.

## Frozen means frozen

CI's `drivers` group runs `grade.py --check` with every built `mo`: every `SHA256SUMS` must match
its files, every reference must pass, every broken solution must check and fail a hidden test,
and every start folder must fail too, a fix task's at `mo check` and the others' at a hidden
test, with a failing-test task's start failing its named test and no other. The `static` group
checks that the executable grammar accepts every reference and broken solution. So no task needs a
form this build hasn't finished: the references avoid `List.sum`, `to_set`, `to_big` and the
checked and saturating arithmetic other than `checked_add`, which `mo check` reports as E1150 at
this commit. Once a session has seen a task, neither its text nor its files change: a change is
a new decision and a new task, and `grade.py --freeze TASK` writes its new hashes.

## Held-out tasks

Tasks 05, 10, 15 and 20 were written on 22 October 2026 for benchmark v0's first round, and no
session, grader or published document has seen them. Protocol section 6 asks that they be
written after the release of every model taking part, so a round that uses a model released
after that date writes new ones first. They are reported apart from the others. After the round
they are ordinary tasks, and the next round writes four new held-out ones.
