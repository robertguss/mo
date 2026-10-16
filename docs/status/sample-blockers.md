# Sample blockers after C7

Regenerated from actual `mo check --json examples/<sample>.mo` output on
`bfcdedcd` (I1/PR81), `75e6db61` (LW5/PR82) and the C7 implementation.
Every totals record reports `dropped: 0`. Examples are unchanged.

`FileError`'s text form (`PATH: no such file`, `PATH: not allowed`,
`PATH: not text`, `PATH: REASON`) is accepted through check, lowering and run.
All four `E1103 FileError's library text function is unavailable` diagnostics
(`expenses.mo:92:34`, `find.mo:104:32`, `todo.mo:35:26`, `wc.mo:52:30`) are
eliminated:
- `expenses.mo` drops from 4 to 3 errors.
- `wc.mo` drops from 2 to 1 error.
- `find.mo` passes `mo_check` with 0 checker diagnostics and reaches `mo_lower`,
  where `console.error` at `92:7` reports `E1150` (1 error total).
- `todo.mo` clears the `E1103` halt in its first function (`to_text` at `35:26`),
  allowing `check_program` to check `parse_items`, `apply` and `main` and expose
  their existing `Option.or_fail` and `List.map` `E0050` diagnostics (1→4).

## Counts

| Sample | Main / I1 | LW5 | C7 | Change (LW5→C7) |
| --- | ---: | ---: | ---: | ---: |
| account.mo | 1 | 1 | 1 | +0 |
| expenses.mo | 2 | 4 | 3 | -1 |
| find.mo | 1 | 1 | 1 | +0 |
| greet.mo | 0 | 0 | 0 | +0 |
| inventory.mo | 2 | 5 | 5 | +0 |
| job-queue.mo | 1 | 3 | 3 | +0 |
| json-query.mo | 1 | 4 | 4 | +0 |
| kv.mo | 1 | 1 | 1 | +0 |
| notes-tool.mo | 1 | 1 | 1 | +0 |
| rate-limit.mo | 1 | 1 | 1 | +0 |
| todo.mo | 1 | 1 | 4 | +3 |
| wc.mo | 2 | 2 | 1 | -1 |
| **Total** | **14** | **24** | **25** | **+1** |

## Current diagnostics

| Sample | Location | Code | Diagnostic | Source |
| --- | --- | --- | --- | --- |
| account.mo | 1:1 | E0079 | the entry module has no main function | `## The core of a bank account: deposits, withdrawals that can't overdraw, and` |
| expenses.mo | 38:24 | E0050 | no visible function has this dot-call name | `case line.split(",").map(fn(field) field.trim end)` |
| expenses.mo | 57:17 | E0050 | no visible function has this dot-call name | `sums = sums.update(expense.category, 0, fn(sum) sum + expense.amount end)` |
| expenses.mo | 68:16 | E0050 | no visible function has this dot-call name | `names = rows.map(fn(row) row.0 end)` |
| find.mo | 92:7 | E1150 | this native call is not lowered yet | `console.error("find: #{problem}")` |
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
| notes-tool.mo | 1:1 | E0079 | the entry module has no main function | `## The handler of an MCP tool that searches Markdown notes. It is handed one` |
| rate-limit.mo | 66:14 | E1103 | this body rule is not implemented by the staged checker | `elapsed = (now - bucket.updated).in_seconds.max(0)` |
| todo.mo | 71:45 | E0050 | no visible function has this dot-call name | `rest = try line.drop_prefix("[ ] ").or_fail(BadLine(number: index + 1))` |
| todo.mo | 85:9 | E0050 | no visible function has this dot-call name | `items.map(fn(item) "#{box(item)} #{item.text}\n" end).join("")` |
| todo.mo | 110:39 | E0050 | no visible function has this dot-call name | `var item = try items.get(n - 1).or_fail(NoSuchItem(number: n))` |
| todo.mo | 120:42 | E0050 | no visible function has this dot-call name | `command = try parse_command(args.list).or_fail(Usage)` |
| wc.mo | 20:29 | E0055 | a call's arguments don't match its parameters | `lines: text.chars.count(fn(c) c == '\n' end),` |

## Newly exposed blockers

- find: passes `mo_check` completely; `mo_lower` reports `E1150` on
  `Console.error` at `92:7`.
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
