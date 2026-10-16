# Sample blockers after C1

Regenerated from actual `mo check --json examples/<sample>.mo` output on
`bfcdedcd` (I1/PR81), `75e6db61` (LW5/PR82), `02395dbd` (C7/PR83),
`09f4c445` (F8/PR84) and C1. Every totals record reports `dropped: 0`.
Examples are unchanged.

Per D250, a module without `main` is a library module: `mo check` and `mo test`
accept it, and `E0079` is reported only by `mo run`:
- `account.mo` drops from 1 to 0 errors and passes `mo check` (including its
  anchors).
- `job-queue.mo`, `notes-tool.mo` and `rate-limit.mo` get no `E0079` from
  `mo check`.
- `notes-tool.mo` now passes `mo_check` and reaches `mo_lower`, which reports
  `E1150` on `input.query.trim` at `45:22` (handled by F14).

## Counts

| Sample | Main / I1 | LW5 | C7 | F8 | C1 | Change (F8→C1) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| account.mo | 1 | 1 | 1 | 1 | 0 | -1 |
| expenses.mo | 2 | 4 | 3 | 3 | 3 | +0 |
| find.mo | 1 | 1 | 1 | 0 | 0 | +0 |
| greet.mo | 0 | 0 | 0 | 0 | 0 | +0 |
| inventory.mo | 2 | 5 | 5 | 5 | 5 | +0 |
| job-queue.mo | 1 | 3 | 3 | 3 | 3 | +0 |
| json-query.mo | 1 | 4 | 4 | 4 | 4 | +0 |
| kv.mo | 1 | 1 | 1 | 1 | 1 | +0 |
| notes-tool.mo | 1 | 1 | 1 | 1 | 1 | +0 |
| rate-limit.mo | 1 | 1 | 1 | 1 | 1 | +0 |
| todo.mo | 1 | 1 | 4 | 4 | 4 | +0 |
| wc.mo | 2 | 2 | 1 | 1 | 1 | +0 |
| **Total** | **14** | **24** | **25** | **24** | **23** | **-1** |

## Current diagnostics

| Sample | Location | Code | Diagnostic | Source |
| --- | --- | --- | --- | --- |
| expenses.mo | 38:24 | E0050 | no visible function has this dot-call name | `case line.split(",").map(fn(field) field.trim end)` |
| expenses.mo | 57:17 | E0050 | no visible function has this dot-call name | `sums = sums.update(expense.category, 0, fn(sum) sum + expense.amount end)` |
| expenses.mo | 68:16 | E0050 | no visible function has this dot-call name | `names = rows.map(fn(row) row.0 end)` |
| inventory.mo | 30:9 | E0050 | no visible function has this dot-call name | `items.sort_by_desc(key).take(n)` |
| inventory.mo | 42:21 | E0050 | no visible function has this dot-call name | `counts = counts.update(key(item), 0, fn(n) n + 1 end)` |
| inventory.mo | 72:22 | E0050 | no visible function has this dot-call name | `laid_out = columns.map(fn(column) pad_column(rows, column) end)` |
| inventory.mo | 81:38 | E0050 | no visible function has this dot-call name | `cells = [column.title].concat(rows.map(column.cell))` |
| inventory.mo | 114:52 | E0050 | no visible function has this dot-call name | `picks = first_by(products, fn(p) p.category end).map(fn(p) p.name end)` |
| job-queue.mo | 70:39 | E0050 | no visible function has this dot-call name | `until = try now.checked_add(length).or_fail(BadLease(length: length))` |
| job-queue.mo | 135:32 | E0050 | no visible function has this dot-call name | `job = try queue.jobs.get(id).or_fail(NoSuchJob(id: id))` |
| job-queue.mo | 157:20 | E0050 | no visible function has this dot-call name | `Queue(jobs: jobs.map(fn(job) (job.id, job) end).to_map, next_id: jobs.size + 1)` |
| json-query.mo | 78:25 | E0050 | no visible function has this dot-call name | `c = try chars.get(at).or_fail(UnexpectedEnd)` |
| json-query.mo | 137:31 | E0050 | no visible function has this dot-call name | `value = try text.to_decimal.or_fail(BadNumber(at: at))` |
| json-query.mo | 211:21 | E0050 | no visible function has this dot-call name | `lines = items.map(fn(item) "#{inner}#{pretty(item, inner)}" end)` |
| json-query.mo | 228:19 | E0050 | no visible function has this dot-call name | `"\"#{text.chars.map(escape_char).join("")}\""` |
| kv.mo | 79:30 | E0050 | no visible function has this dot-call name | `n = try seconds.to_int.or_fail(NotAnInteger(text: seconds))` |
| notes-tool.mo | 45:22 | E1150 | this native call is not lowered yet | `query = input.query.trim.ascii_lowercase` |
| rate-limit.mo | 66:14 | E1103 | this body rule is not implemented by the staged checker | `elapsed = (now - bucket.updated).in_seconds.max(0)` |
| todo.mo | 71:45 | E0050 | no visible function has this dot-call name | `rest = try line.drop_prefix("[ ] ").or_fail(BadLine(number: index + 1))` |
| todo.mo | 85:9 | E0050 | no visible function has this dot-call name | `items.map(fn(item) "#{box(item)} #{item.text}\n" end).join("")` |
| todo.mo | 110:39 | E0050 | no visible function has this dot-call name | `var item = try items.get(n - 1).or_fail(NoSuchItem(number: n))` |
| todo.mo | 120:42 | E0050 | no visible function has this dot-call name | `command = try parse_command(args.list).or_fail(Usage)` |
| wc.mo | 20:29 | E0055 | a call's arguments don't match its parameters | `lines: text.chars.count(fn(c) c == '\n' end),` |

## Newly exposed blockers

- account: passes `mo check` (including its anchors) with 0 errors.
- find: passes `mo check`, `mo test` and `mo run` with 0 errors.
- notes-tool: clearing `E0079` lets `mo check` reach `mo_lower`, exposing
  `E1150` on `Text.trim` (`45:22`).
- todo: clearing `E1103` in `to_text` (`35:26`) allows `check_program` to
  continue into `parse_items`, `apply` and `main`, exposing `Option.or_fail`
  (`71:45`, `110:39`, `120:42`) and `List.map` (`85:9`).
- expenses: `Map.update` (57:17), `List.map` (38:24, 68:16); its `FileError`
  text blocker at `92:34` is cleared.
- wc: `List.count` with a predicate (20:29); its `FileError` text blocker at
  `52:30` is cleared.
- inventory: `Map.update` (42:21), `List.map` (72:22, 81:38, 114:52),
  `sort_by_desc` (30:9).
- job-queue: `Option.or_fail` (70:39, 135:32), `List.map` (157:20).
- json-query: `Option.or_fail` (78:25, 137:31), `List.map` (211:21, 228:19).
- kv: `Option.or_fail` (79:30).
- rate-limit: Time arithmetic around `now - bucket.updated` (66:14).

The existing `infer_function` sets `halt` for implementation codes (E1000 and
above). Its replacement library errors E0050 allow `check_program` to continue
through independent bodies. No diagnostic is hidden to keep the former totals.

## Regeneration and evidence

Build `mo`, then run:

```sh
build/clang/debug/mo check --json examples/<sample>.mo
```

Read `errors` from each totals record and assert `dropped == 0`. Historical
LW5 and I1 verification records remain in [lw5-verification.md](lw5-verification.md)
and [i1-verification.md](i1-verification.md).

