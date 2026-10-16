# Sample blockers after MO-83 (List.all?)

Regenerated from actual `mo check --json examples/<sample>.mo` output on
`bfcdedcd` (I1/PR81), `75e6db61` (LW5/PR82), `02395dbd` (C7/PR83),
`09f4c445` (F8/PR84), `1ac2fe8e` (C1/PR85), `0b8589ee` (C4/PR86),
`a2924f53` (C5/PR87), `dab7da3a` (C8/PR88), `b9fbb0e1` (F7/PR89),
`d7e3abfa` (C3/PR90), `bc0fff4c` (F16/PR91), `4cf4f81c` (F14/PR92),
`41d6944` (F11/PR93), `3ad6c55` (MO-31/PR97) and MO-83. Every totals record
reports `dropped: 0`. Examples are unchanged.

With `List.all?` checked, lowered and run (plus lowering dispatch for
`Console.read_line`, `Console.fake_input`, `Int.checked_add`, `Map.remove` and
`Map.values`):
- `examples/kv.mo` passes `mo check` (including its `allows` and `forbids`
  anchors), `mo run`, and all 15 examples and 2 tests with 0 errors.
- Total diagnostics across all 12 samples fall from 6 to 5, and 7 of 12
  samples now pass `mo check` (`account.mo`, `find.mo`, `greet.mo`, `kv.mo`,
  `notes-tool.mo`, `rate-limit.mo`, `wc.mo`).
- `todo.mo`'s `main` returning `Result(Unit, TodoError)` was already cleared by
  #94 (MO-26); it now passes `mo_check` and stops in lowering at
  `Files.write_text` (`132:9`, E1150).

## Counts

| Sample | Main / I1 | LW5 | C7 | F8 | C1 | C4 | C5 | C8 | F7 | C3 | F16 | F14 | F11 | MO-31 | MO-83 | Change (MO-31→MO-83) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| account.mo | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | +0 |
| expenses.mo | 2 | 4 | 3 | 3 | 3 | 3 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 1 | 1 | +0 |
| find.mo | 1 | 1 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | +0 |
| greet.mo | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | +0 |
| inventory.mo | 2 | 5 | 5 | 5 | 5 | 3 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | +0 |
| job-queue.mo | 1 | 3 | 3 | 3 | 3 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 1 | 1 | +0 |
| json-query.mo | 1 | 4 | 4 | 4 | 4 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 1 | 1 | +0 |
| kv.mo | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 0 | -1 |
| notes-tool.mo | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 0 | +0 |
| rate-limit.mo | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 0 | 0 | +0 |
| todo.mo | 1 | 1 | 4 | 4 | 4 | 3 | 3 | 3 | 3 | 3 | 3 | 3 | 3 | 1 | 1 | +0 |
| wc.mo | 2 | 2 | 1 | 1 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | +0 |
| **Total** | **14** | **24** | **25** | **24** | **23** | **17** | **14** | **14** | **13** | **13** | **12** | **11** | **11** | **6** | **5** | **-1** |

## Current diagnostics

| Sample | Location | Code | Diagnostic | Source |
| --- | --- | --- | --- | --- |
| expenses.mo | 70:56 | E0050 | no visible function has this dot-call name | `name_width = names.map(fn(name) name.char_count end).max or 0` |
| inventory.mo | 72:48 | E1103 | this generic capture's capability membership isn't checked | `laid_out = columns.map(fn(column) pad_column(rows, column) end)` |
| job-queue.mo | 71:26 | E0050 | no visible function has this dot-call name | `case queue.jobs.values.find(fn(job) job.state == Ready end)` |
| json-query.mo | 62:28 | E1150 | this native call is not lowered yet | `Some(c) if c == '-' or c.ascii_digit?: number_at(chars, at)` |
| todo.mo | 132:9 | E1150 | this native call is not lowered yet | `try files.write_text(file, format(items))` |

## Newly exposed blockers

- expenses: `Option.or_fail` (`41:37`) clears; only `List.max` (`70:56`) remains.
- inventory: unchanged; `E1103` on the generic `rows: List(T)` capture in
  `pad_table` (`72:48`).
- job-queue: both `Option.or_fail` calls clear, exposing `List.find` (`71:26`).
- json-query: both `Option.or_fail` calls clear; the sample now checks and stops
  in lowering at `Char.ascii_digit?` (`62:28`, E1150).
- kv: `List.all?` (`90:29`) clears; passes `mo check` (including its anchors),
  `mo run`, and its examples and tests with 0 errors.
- todo: all three `Option.or_fail` calls and `main` returning
  `Result(Unit, TodoError)` (#94) clear; the sample now checks and stops in
  lowering at `Files.write_text` (`132:9`, E1150).

No diagnostic is hidden to keep the former totals.

## Regeneration and evidence

Build `mo`, then run:

```sh
build/clang/debug/mo check --json examples/<sample>.mo
```

Read `errors` from each totals record and assert `dropped == 0`. Historical
LW5 and I1 verification records remain in [lw5-verification.md](lw5-verification.md)
and [i1-verification.md](i1-verification.md).
