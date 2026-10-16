# Sample blockers after MO-31 (Option.or_fail)

Regenerated from actual `mo check --json examples/<sample>.mo` output on
`bfcdedcd` (I1/PR81), `75e6db61` (LW5/PR82), `02395dbd` (C7/PR83),
`09f4c445` (F8/PR84), `1ac2fe8e` (C1/PR85), `0b8589ee` (C4/PR86),
`a2924f53` (C5/PR87), `dab7da3a` (C8/PR88), `b9fbb0e1` (F7/PR89),
`d7e3abfa` (C3/PR90), `bc0fff4c` (F16/PR91), `4cf4f81c` (F14/PR92),
`41d6944` (F11/PR93) and MO-31. Every totals record reports `dropped: 0`.
Examples are unchanged.

With `Option.or_fail` checked, lowered and run:
- All nine `Option.or_fail` diagnostics clear. Each of the six failing samples
  now stops at exactly one diagnostic, none of them `or_fail`.
- Total diagnostics across all 12 samples fall from 11 to 6, and 6 of 12
  samples still pass `mo check` (`account.mo`, `find.mo`, `greet.mo`,
  `notes-tool.mo`, `rate-limit.mo`, `wc.mo`).
- `json-query.mo` now passes checking and stops in lowering (E1150).

## Counts

| Sample | Main / I1 | LW5 | C7 | F8 | C1 | C4 | C5 | C8 | F7 | C3 | F16 | F14 | F11 | MO-31 | Change (F11→MO-31) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| account.mo | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | +0 |
| expenses.mo | 2 | 4 | 3 | 3 | 3 | 3 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 1 | -1 |
| find.mo | 1 | 1 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | +0 |
| greet.mo | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | +0 |
| inventory.mo | 2 | 5 | 5 | 5 | 5 | 3 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | +0 |
| job-queue.mo | 1 | 3 | 3 | 3 | 3 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 1 | -1 |
| json-query.mo | 1 | 4 | 4 | 4 | 4 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 1 | -1 |
| kv.mo | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | +0 |
| notes-tool.mo | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | +0 |
| rate-limit.mo | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 0 | +0 |
| todo.mo | 1 | 1 | 4 | 4 | 4 | 3 | 3 | 3 | 3 | 3 | 3 | 3 | 3 | 1 | -2 |
| wc.mo | 2 | 2 | 1 | 1 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | +0 |
| **Total** | **14** | **24** | **25** | **24** | **23** | **17** | **14** | **14** | **13** | **13** | **12** | **11** | **11** | **6** | **-5** |

## Current diagnostics

| Sample | Location | Code | Diagnostic | Source |
| --- | --- | --- | --- | --- |
| expenses.mo | 70:56 | E0050 | no visible function has this dot-call name | `name_width = names.map(fn(name) name.char_count end).max or 0` |
| inventory.mo | 72:48 | E1103 | this generic capture's capability membership isn't checked | `laid_out = columns.map(fn(column) pad_column(rows, column) end)` |
| job-queue.mo | 71:26 | E0050 | no visible function has this dot-call name | `case queue.jobs.values.find(fn(job) job.state == Ready end)` |
| json-query.mo | 62:28 | E1150 | this native call is not lowered yet | `Some(c) if c == '-' or c.ascii_digit?: number_at(chars, at)` |
| kv.mo | 90:29 | E0050 | no visible function has this dot-call name | `result.0.entries.values.all?(fn(entry) entry.expires is Some(at) implies at > now end)` |
| todo.mo | 119:4 | E1103 | this main result convention is not implemented | `fn main(args: Args, console: Console, files: Files) : Result(Unit, TodoError)` |

## Newly exposed blockers

- expenses: `Option.or_fail` (`41:37`) clears; only `List.max` (`70:56`) remains.
- inventory: unchanged; `E1103` on the generic `rows: List(T)` capture in
  `pad_table` (`72:48`).
- job-queue: both `Option.or_fail` calls clear, exposing `List.find` (`71:26`).
- json-query: both `Option.or_fail` calls clear; the sample now checks and stops
  in lowering at `Char.ascii_digit?` (`62:28`, E1150).
- kv: `Option.or_fail` (`79:30`) clears, exposing `List.all?` (`90:29`).
- todo: all three `Option.or_fail` calls clear, exposing `main` returning
  `Result(Unit, E)` (`119:4`, E1103), which is F5 / MO-26.

No diagnostic is hidden to keep the former totals.

## Regeneration and evidence

Build `mo`, then run:

```sh
build/clang/debug/mo check --json examples/<sample>.mo
```

Read `errors` from each totals record and assert `dropped == 0`. Historical
LW5 and I1 verification records remain in [lw5-verification.md](lw5-verification.md)
and [i1-verification.md](i1-verification.md).
