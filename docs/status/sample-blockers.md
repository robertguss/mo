# Sample blockers after MO-88 (generic captures checked at use)

Regenerated from actual `mo check --json examples/<sample>.mo` output on
`bfcdedcd` (I1/PR81), `75e6db61` (LW5/PR82), `02395dbd` (C7/PR83),
`09f4c445` (F8/PR84), `1ac2fe8e` (C1/PR85), `0b8589ee` (C4/PR86),
`a2924f53` (C5/PR87), `dab7da3a` (C8/PR88), `b9fbb0e1` (F7/PR89),
`d7e3abfa` (C3/PR90), `bc0fff4c` (F16/PR91), `4cf4f81c` (F14/PR92),
`41d6944` (F11/PR93), `3ad6c55` (MO-31/PR97), `5375209` (MO-83/PR101),
`0d3764c` (MO-84/PR106), MO-85/PR102, MO-86/PR103, MO-87/PR104 and MO-88.
Every totals record reports `dropped: 0`. Examples are unchanged.

MO-91 part 1 lets `mo test` generate `Map` and `Set` promise inputs. No sample
changes status: account, job-queue, kv, notes-tool and rate-limit still stop at
E1400, because each promise subject also takes a
type exposed without its insides (`Store` in `kv.mo`, `Limiter` in
`rate-limit.mo`, `Queue` in `job-queue.mo`, `Account` in `account.mo`), which
needs module builders (reference 12.2). `notes-tool.mo` also needs fake
capabilities as inputs.

MO-91 part 2 (fake capabilities as promise inputs) clears `notes-tool.mo`: its only promise
subject takes a `ReadFiles`, and `mo test` now passes all 4 of its observations. Its other
four siblings still need module builders.

MO-91 part 3 (hidden types built through their module) clears the last four. `mo test` builds a
`Store`, `Limiter`, `Queue` or `Account` from its module's exposed constants and functions
(`empty_store` and `apply`, `new_limiter` and `request`, `empty` and the queue operations,
`open`, `deposit`, `withdraw` and `transfer`), and every observation passes: account 16,
job-queue 13, kv 18 and rate-limit 6. All 12 samples now pass `mo check` and plain `mo test`.

MO-111 lowers the remaining reference section 14 `Int`, `Float` and `Bytes` functions.
These were library gaps, not blockers in the twelve samples, so the sample totals do not change.
The `int-natives`, `float-natives` and `bytes-natives` lower-source fixtures cover them.

MO-88 checks a generic capture's capability membership where the generic
function is used (E0091 only when the type argument holds a capability):
- `examples/inventory.mo` passes `mo check`, `mo run`, and its 8 examples and
  2 tests with 0 errors.
- Total diagnostics across all 12 samples fall from 1 to 0, and all 12 samples
  now pass `mo check`.

Earlier, MO-87 lowered `Files.write_text`:
- `examples/todo.mo` passes `mo check`, runs `add`, `done`, `list` and
  `remove` against a real folder, and passes all 15 examples and tests with 0
  errors.
- Total diagnostics across all 12 samples fall from 2 to 1, and 11 of 12
  samples now pass `mo check` (all but `inventory.mo`).

Earlier, MO-86 dispatched every `Char` primitive (`ascii_digit?`, `ascii_letter?`,
`ascii_white_space?`, `ascii_lowercase`, `ascii_uppercase`, `code`, `to_text`),
`Int.to_char` and the remaining `Map` primitives (`empty?`, `has?`, `keys`,
`entries`) to Mo Core, all of which Core and the VM already had:
- `examples/json-query.mo` passes `mo check`, runs on standard input, and
  passes all 15 examples and 1 test with 0 errors.
- Total diagnostics across all 12 samples fall from 3 to 2, and 10 of 12
  samples now pass `mo check` (`account.mo`, `expenses.mo`, `find.mo`,
  `greet.mo`, `job-queue.mo`, `json-query.mo`, `kv.mo`, `notes-tool.mo`,
  `rate-limit.mo`, `wc.mo`).

Earlier, with `List.find` checked, lowered and run (plus lowering for `List.to_map`,
`Map.has?` and `Map.keys`, which `examples/job-queue.mo` reaches once
`List.find` clears):
- `examples/job-queue.mo` passes `mo check` (including its anchors), and its 8
  examples and 1 test pass; its promises still stop at E1400 (M9).
- Total diagnostics across all 12 samples fall from 4 to 3, and 9 of 12
  samples now pass `mo check` (`account.mo`, `expenses.mo`, `find.mo`,
  `greet.mo`, `job-queue.mo`, `kv.mo`, `notes-tool.mo`, `rate-limit.mo`,
  `wc.mo`).

Earlier, with `List.max` checked, lowered and run (plus lowering for `Map.entries` and
`List.zip`, which `examples/expenses.mo` reaches once `List.max` clears):
- `examples/expenses.mo` passes `mo check`, `mo run` on a CSV, and its 9
  examples and tests with 0 errors.
- Total diagnostics across all 12 samples fall from 5 to 4, and 8 of 12
  samples now pass `mo check` (`account.mo`, `expenses.mo`, `find.mo`,
  `greet.mo`, `kv.mo`, `notes-tool.mo`, `rate-limit.mo`, `wc.mo`).

Earlier, with `List.all?` checked, lowered and run (plus lowering dispatch for
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

| Sample | Main / I1 | LW5 | C7 | F8 | C1 | C4 | C5 | C8 | F7 | C3 | F16 | F14 | F11 | MO-31 | MO-83 | MO-84 | MO-85 | MO-86 | MO-87 | MO-88 | Change (MO-87→MO-88) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| account.mo | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | +0 |
| expenses.mo | 2 | 4 | 3 | 3 | 3 | 3 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 1 | 1 | 0 | 0 | 0 | 0 | 0 | +0 |
| find.mo | 1 | 1 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | +0 |
| greet.mo | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | +0 |
| inventory.mo | 2 | 5 | 5 | 5 | 5 | 3 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 0 | -1 |
| job-queue.mo | 1 | 3 | 3 | 3 | 3 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 1 | 1 | 1 | 0 | 0 | 0 | 0 | +0 |
| json-query.mo | 1 | 4 | 4 | 4 | 4 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | +0 |
| kv.mo | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | +0 |
| notes-tool.mo | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | +0 |
| rate-limit.mo | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | +0 |
| todo.mo | 1 | 1 | 4 | 4 | 4 | 3 | 3 | 3 | 3 | 3 | 3 | 3 | 3 | 1 | 1 | 1 | 1 | 1 | 0 | 0 | +0 |
| wc.mo | 2 | 2 | 1 | 1 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | +0 |
| **Total** | **14** | **24** | **25** | **24** | **23** | **17** | **14** | **14** | **13** | **13** | **12** | **11** | **11** | **6** | **5** | **4** | **3** | **2** | **1** | **0** | **-1** |

## Current diagnostics

None: all 12 samples pass `mo check` with 0 errors.

## Unblocked forms (history)

- expenses: `List.max` (`70:56`) clears, exposing `Map.entries` (`59:7`) and
  `List.zip` (`73:25`) in lowering; all three now lower. The sample passes
  `mo check`, `mo run` and its 9 examples and tests with 0 errors.
- inventory: the generic `rows: List(T)` capture in `pad_table` (`72:48`) is
  now checked where `pad_table` is used, so `E1103` clears; the sample passes
  `mo check`, `mo run` and its 10 examples and tests with 0 errors.
- job-queue: `List.find` (`71:26`) clears. Lowering then needed `List.to_map`,
  `Map.has?` and `Map.keys`, which now lower to their existing Core primitives.
  The sample passes `mo check` (including its anchors) and its 8 examples and 1
  test pass.
- json-query: `Char.ascii_digit?` (`62:28`) clears, and so do the `Int.to_char`
  and `Map.empty?` calls behind it; the sample runs and passes its examples and
  tests with 0 errors.
- kv: `List.all?` (`90:29`) clears; passes `mo check` (including its anchors),
  `mo run`, and its examples and tests with 0 errors.
- account, job-queue, kv and rate-limit: their promises take a type exposed without its
  insides, which `mo test` now builds through the type's module (`src/tools/builders.c`).
  Each sample passes plain `mo test`.
- notes-tool: its promise takes a `ReadFiles`, which promises now receive as a generated
  fake (`src/tools/fakes.c`). `mo test` passes the promise and its 3 tests, so the sample
  passes plain `mo test`. `Env.get` and `Random.int` do not lower yet, so a promise whose
  body calls them still stops at E1150; no sample does.
- todo: `Files.write_text` (`132:9`) clears; the sample passes `mo check`,
  `mo run` and its 15 examples and tests with 0 errors.

No diagnostic is hidden to keep the former totals.

## Regeneration and evidence

Build `mo`, then run:

```sh
build/clang/debug/mo check --json examples/<sample>.mo
```

Read `errors` from each totals record and assert `dropped == 0`. Historical
LW5 and I1 verification records remain in [lw5-verification.md](lw5-verification.md)
and [i1-verification.md](i1-verification.md).
