# Sample blockers after LW5

Regenerated from actual `mo check --json examples/<sample>.mo` output on
`bfcdedcd` (current main including I1/PR81) and the LW5 implementation.
Every totals record reports `dropped: 0`. Examples are unchanged.

Map literals are accepted in all six affected samples. Checking reaches later
independent functions and exposes existing library gaps. The requested
**no-increase error-count criterion is not met**: four sample counts increase.
This is pending acceptance clarification; diagnostics are retained and tasks C4/C5
remain out of scope.

## Counts

| Sample | Main / I1 | LW5 | Change |
| --- | ---: | ---: | ---: |
| account.mo | 1 | 1 | +0 |
| expenses.mo | 2 | 4 | +2 |
| find.mo | 1 | 1 | +0 |
| greet.mo | 0 | 0 | +0 |
| inventory.mo | 2 | 5 | +3 |
| job-queue.mo | 1 | 3 | +2 |
| json-query.mo | 1 | 4 | +3 |
| kv.mo | 1 | 1 | +0 |
| notes-tool.mo | 1 | 1 | +0 |
| rate-limit.mo | 1 | 1 | +0 |
| todo.mo | 1 | 1 | +0 |
| wc.mo | 2 | 2 | +0 |
| **Total** | **14** | **24** | **+10** |

## Current diagnostics

| Sample | Location | Code | Diagnostic | Source |
| --- | --- | --- | --- | --- |
| account.mo | 1:1 | E0079 | the entry module has no main function | `## The core of a bank account: deposits, withdrawals that can't overdraw, and` |
| expenses.mo | 38:24 | E0050 | no visible function has this dot-call name | `case line.split(",").map(fn(field) field.trim end)` |
| expenses.mo | 57:17 | E0050 | no visible function has this dot-call name | `sums = sums.update(expense.category, 0, fn(sum) sum + expense.amount end)` |
| expenses.mo | 68:16 | E0050 | no visible function has this dot-call name | `names = rows.map(fn(row) row.0 end)` |
| expenses.mo | 92:34 | E1103 | FileError's library text function is unavailable | `console.error("expenses: #{failure}")` |
| find.mo | 104:32 | E1103 | FileError's library text function is unavailable | `console.error("find: #{failure}")` |
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
| todo.mo | 35:26 | E1103 | FileError's library text function is unavailable | `Storage(failure): "#{failure}"` |
| wc.mo | 20:29 | E0055 | a call's arguments don't match its parameters | `lines: text.chars.count(fn(c) c == '\n' end),` |
| wc.mo | 52:30 | E1103 | FileError's library text function is unavailable | `console.error("wc: #{failure}")` |

## Newly exposed blockers

- expenses: `Map.update` (57:17), `List.map` (68:16), and unavailable FileError
  library text (92:34); its earlier `List.map` at 38:24 remains.
- inventory: `Map.update` (42:21), `List.map` (72:22, 81:38, 114:52);
  `sort_by_desc` (30:9) remains.
- job-queue: `Option.or_fail` (70:39, 135:32), `List.map` (157:20).
- json-query: `Option.or_fail` (78:25, 137:31), `List.map` (211:21, 228:19).
- kv: `Option.or_fail` (79:30).
- rate-limit: Time arithmetic around `now - bucket.updated` (66:14) is the next staged
  body form after `{}` is cleared.

The existing `infer_function` sets `halt` for implementation codes (E1000 and
above), including the map blocker E1103. Its replacement library errors E0050
allow `check_program` to continue through independent bodies. The increase comes
from that existing halt policy after clearing the first map blocker. It is not evidence that those library
operations are implemented. No diagnostic is hidden to keep the former totals.

## Regeneration and evidence

Build clean main and LW5 in independent workspaces, then run:

```sh
build/clang/debug/mo check --json examples/<sample>.mo
```

Read `errors` from each totals record and assert `dropped == 0`. Baseline and
LW5 raw JSON are retained outside the build tree in the dedicated task’s
`evidence/linux-baseline-samples.json`, `evidence/linux-lw5-samples.json`
and `evidence/linux-lw5-sample-comparison.json` (Mac capture: `lw5-samples.json`).
Commands, full-gate evidence and unresolved questions are recorded in
[lw5-verification.md](lw5-verification.md). Historical I1 verification remains in
[i1-verification.md](i1-verification.md).
