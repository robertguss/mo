# Sample blockers after C3

Regenerated from actual `mo check --json examples/<sample>.mo` output on
`bfcdedcd` (I1/PR81), `75e6db61` (LW5/PR82), `02395dbd` (C7/PR83),
`09f4c445` (F8/PR84), `1ac2fe8e` (C1/PR85), `0b8589ee` (C4/PR86),
`a2924f53` (C5/PR87), `dab7da3a` (C8/PR88), `b9fbb0e1` (F7/PR89) and C3. Every
totals record reports `dropped: 0`. Examples are unchanged.

With `Time` and `Duration` arithmetic checked per Reference §3.9 (`Time - Time -> Duration`,
`Time + Duration -> Time`, `Time - Duration -> Time`, `Duration + Duration -> Duration`,
`Duration - Duration -> Duration`, `Duration * Int -> Duration`, `-Duration -> Duration`)
and `Int` duration constructors, `Time`/`Duration` methods and `Clock` primitives
lowered to their Core primitives:
- `examples/rate-limit.mo` clears `E1103` at `66:14` (`now - bucket.updated`) and
  exposes `E1150` at `66:13` (`Decimal.max` during anchor lowering).
- Total diagnostics across all 12 samples remain 13, with 0 `E1103` errors left
  in `examples/rate-limit.mo`.

## Counts

| Sample | Main / I1 | LW5 | C7 | F8 | C1 | C4 | C5 | C8 | F7 | C3 | Change (F7→C3) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| account.mo | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | +0 |
| expenses.mo | 2 | 4 | 3 | 3 | 3 | 3 | 2 | 2 | 2 | 2 | +0 |
| find.mo | 1 | 1 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | +0 |
| greet.mo | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | +0 |
| inventory.mo | 2 | 5 | 5 | 5 | 5 | 3 | 1 | 1 | 1 | 1 | +0 |
| job-queue.mo | 1 | 3 | 3 | 3 | 3 | 2 | 2 | 2 | 2 | 2 | +0 |
| json-query.mo | 1 | 4 | 4 | 4 | 4 | 2 | 2 | 2 | 2 | 2 | +0 |
| kv.mo | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | +0 |
| notes-tool.mo | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | +0 |
| rate-limit.mo | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | +0 |
| todo.mo | 1 | 1 | 4 | 4 | 4 | 3 | 3 | 3 | 3 | 3 | +0 |
| wc.mo | 2 | 2 | 1 | 1 | 1 | 1 | 1 | 1 | 0 | 0 | +0 |
| **Total** | **14** | **24** | **25** | **24** | **23** | **17** | **14** | **14** | **13** | **13** | **+0** |

## Current diagnostics

| Sample | Location | Code | Diagnostic | Source |
| --- | --- | --- | --- | --- |
| expenses.mo | 41:37 | E0050 | no visible function has this dot-call name | `value = try amount.to_decimal.or_fail(BadAmount(text: amount))` |
| expenses.mo | 70:56 | E0050 | no visible function has this dot-call name | `name_width = names.map(fn(name) name.char_count end).max or 0` |
| inventory.mo | 72:48 | E1103 | this generic capture's capability membership isn't checked | `laid_out = columns.map(fn(column) pad_column(rows, column) end)` |
| job-queue.mo | 70:39 | E0050 | no visible function has this dot-call name | `until = try now.checked_add(length).or_fail(BadLease(length: length))` |
| job-queue.mo | 135:32 | E0050 | no visible function has this dot-call name | `job = try queue.jobs.get(id).or_fail(NoSuchJob(id: id))` |
| json-query.mo | 78:25 | E0050 | no visible function has this dot-call name | `c = try chars.get(at).or_fail(UnexpectedEnd)` |
| json-query.mo | 137:31 | E0050 | no visible function has this dot-call name | `value = try text.to_decimal.or_fail(BadNumber(at: at))` |
| kv.mo | 79:30 | E0050 | no visible function has this dot-call name | `n = try seconds.to_int.or_fail(NotAnInteger(text: seconds))` |
| notes-tool.mo | 45:22 | E1150 | this native call is not lowered yet | `query = input.query.trim.ascii_lowercase` |
| rate-limit.mo | 66:13 | E1150 | this native call is not lowered yet | `elapsed = (now - bucket.updated).in_seconds.max(0)` |
| todo.mo | 71:45 | E0050 | no visible function has this dot-call name | `rest = try line.drop_prefix("[ ] ").or_fail(BadLine(number: index + 1))` |
| todo.mo | 110:39 | E0050 | no visible function has this dot-call name | `var item = try items.get(n - 1).or_fail(NoSuchItem(number: n))` |
| todo.mo | 120:42 | E0050 | no visible function has this dot-call name | `command = try parse_command(args.list).or_fail(Usage)` |

## Newly exposed blockers

- account: passes `mo check` (including its anchors) with 0 errors.
- find: passes `mo check`, `mo test` and `mo run` with 0 errors.
- wc: passes `mo check`, `mo test` and `mo run` with 0 errors.
- rate-limit: `E1103` on `now - bucket.updated` (`66:14`) clears; `E1150` on
  `Decimal.max` (`66:13`) is exposed when lowering its anchors.
- expenses: `Map.update` (`57:17`) and `List.sort_by_desc` (`91:8`) clear; only
  `Option.or_fail` (`41:37`) and `List.max` (`70:56`) remain.
- inventory: `List.sort_by_desc` (`30:9`) and `Map.update` (`42:21`) clear; only
  `E1103` on the generic `rows: List(T)` capture in `pad_table` (`72:48`)
  remains.
- job-queue: `.map(...)` at `157:20` clears; only `Option.or_fail` (`70:39`,
  `135:32`) remains.
- json-query: `.map(...)` at `211:21` and `228:19` clears; only `Option.or_fail`
  (`78:25`, `137:31`) remains.
- todo: `.map(...)` at `85:9` clears; only `Option.or_fail` (`71:45`, `110:39`,
  `120:42`) remains.
- notes-tool: `E1150` on `Text.trim` (`45:22`).
- kv: `Option.or_fail` (`79:30`).

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
