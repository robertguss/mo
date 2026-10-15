# Sample blockers

`mo check` rejects all 12 programs in `examples/`. `mo run` rejects all 12 before it looks at the source. Pull request 68, head `2714df12ffabd987b2cd8d1db100aaa4ed160209`, removes 36 of the 81 check diagnostics and adds 7 that the first diagnostic had hidden. None of the 12 exit 0 on either compiler.

The split's acceptance line names `docs/design/sample-blockers.md`. This file is the path the audit was asked to write.

Regenerate the diagnostics with these commands, from a debug build of each commit.

```sh
make
build/cc/debug/mo check --json examples/<name>.mo
build/cc/debug/mo run examples/<name>.mo
```

Main is `3523c4af72188a86a2314c3db4fc14b3668c096d`. Both binaries print `mo 0.0.0`. Each command was run once. The compiler is deterministic, and every JSON totals record has `dropped` 0, so the tables below are the full diagnostic list. The branch's own copies of eight examples differ from main by line wrapping only. Running the pull request binary on those copies produces the same error counts.

The split lists the samples after greet in this order. wc, find, todo, kv, account, expenses, inventory, notes-tool, json-query, rate-limit, job-queue. It calls that list F7 to F17 and does not number each name. This file numbers them in that order. F7 is wc. F17 is job-queue. Greet stays F4.

## How a sample is rejected

`mo check` loads the file, resolves the module, prepares signatures, and calls `mo_infer_function` once per function. That call is in `src/tools/execute.c`. The certified entry `mo_check` in `src/check/program.c` is a different function. The command does not call it.

`infer_function` in `src/check/bodies.c` looks at the function's spec list first. If that list is not empty, the function returns E1103 on the first `examples` or `promise` block and does not infer the body. A function with no spec is inferred until the first unfinished form. Later forms in that function are not reported. One function therefore contributes at most one diagnostic. The 81 diagnostics are 81 functions that hit an unfinished form, not 81 unfinished forms in the sources.

E1103 is `CHECK_UNFINISHED`. The headline on every one of these diagnostics is `this body rule is not implemented by the staged checker`.

A dot call on a built-in type fails in `field_value`, because that type has no source declaration. Pull request 68 resolves the same call when the name is a row in `src/base/prims.def` or `src/base/hostops.def`. Those tables are unchanged between the two commits. The checker change is what clears the call. A name that is not in either table stays unresolved. On the real files the pull request reports that as E0050 for `map`, headline `no visible function has this dot-call name`. `update` and `sort_by_desc` are the same kind of hole. The real files do not reach them, because an earlier form fails first.

`mo run` and `mo emit` return E1400, `TOOL_UNAVAILABLE`, headline `this build cannot run programs yet`, and return before load. The same is true on the pull request. Exit status is 1 for every sample, for both commands, on both compilers.

Some bodies never run, because a spec fails first. The spec probe deletes every `examples` block and every `promise` block from a copy of the file, then runs the pull request binary. Notes below quote the span text from that copy. They do not use its line numbers. The probe still stops at the first unfinished form in each function.

## Summary

| Sample | Task | Main | PR 68 | Still reported on PR 68 | Next form the spec probe reports |
| --- | --- | --- | --- | --- | --- |
| greet.mo | F4 | 2 | 1 | the `examples` block | `name or "world"` |
| wc.mo | F7 | 4 | 2 | two `examples` blocks | `chars.count` of a function |
| find.mo | F8 | 6 | 2 | two `examples` blocks | the probe exits 0 |
| todo.mo | F9 | 6 | 6 | specs, `try`, and the constant `file` | more `try` forms and `List.map` |
| kv.mo | F10 | 10 | 6 | specs, `try`, and `empty_store` | `get(key) or Item(...)` and `max_expiry` |
| account.mo | F11 | 3 | 3 | three `promise` blocks | `max_amount` and `try` |
| expenses.mo | F12 | 6 | 3 | three `examples` blocks | `List.map` and `Map.update` |
| inventory.mo | F13 | 8 | 6 | four `examples` blocks, `List.map`, `products` | `sort_by_desc` and `Map.update` |
| notes-tool.mo | F14 | 1 | 1 | the `promise` block | the constant `max_limit` |
| json-query.mo | F15 | 21 | 10 | five spec blocks and five `try` forms | `List.map` in `pretty` |
| rate-limit.mo | F16 | 4 | 3 | both spec blocks, and Time subtraction | `get(client) or full` |
| job-queue.mo | F17 | 10 | 9 | specs, `try`, two constants, `List.map` | another `try` on `checked_add` |

Main's 81 diagnostics split like this. 36 are an `examples` or `promise` block. 29 are a dot call whose name is already in the primitive or host tables. 7 are text interpolation. 4 are `try`. 3 are a module constant. 1 is `List.map`. 1 is subtraction on Time. 36 plus 29 plus 7 plus 4 plus 3 plus 1 plus 1 is 81.

The pull request clears the 29 primitive and host calls and the 7 interpolations. That leaves 45. Seven further diagnostics then appear in functions whose first error was one of those cleared forms. 45 plus 7 is 52, which is the pull request total. Two of the 52 are E0050. The other 50 are still E1103.

## Cause names used in the tables

**Spec.** The span is an `examples` or `promise` block. The body of that function was not inferred. No task from F2 to F6, R1 to R4, or M1 onward tells the checker to accept the block. F7 to F17, and F4 for greet, each require `mo check` to exit 0, so each of them waits on this gate. M1 runs examples under `mo test`. It does not make `mo check` accept the syntax.

**Interpolation.** The span is a text value that contains `#{`. `MO_S_TEXT_INTERPOLATED` is an unfinished body form. Pull request 68 infers it. The task is F1.

**Native call.** The span is one method or field name, and that name is a row in `prims.def` or `hostops.def`. On main the staged checker returns E1103. F1 clears it.

**Try.** The span is a `try` expression. `MO_S_TRY` falls through to the unfinished body form. F5 is main returning `Result`, which is a different gate and which this command does not report. No listed task adds `try`.

**Constant.** The span is a module-level binding such as `empty_store` or `file`. `identifier` finishes a single function and returns E1103 for anything else. F3 is the constants task. Its text says constants across modules. These bindings are in the same module as the use. See the last section.

**Time minus.** The span is the name `now`. The form is `now - bucket.updated` in `rate-limit.mo`. `binary` returns E1103 for arithmetic on Time, and the span is left on the left operand. `Time.checked_add` is a primitive. The subtraction operator is not a primitive. No listed task adds it.

**List.map.** The span is `map` on a list. There is no `List.map` row in `prims.def`. F1 turns the main diagnostic from E1103 into E0050. F6 is the nearest listed task, because the call takes a function. F6 does not name `map`.

## greet.mo

F4 says `mo check examples/greet.mo` exits 0. The command's failure is the `examples` block on `greeting`, then `console.print` on main. The signature `main(args: Args, console: Console)` is not a diagnostic.

| Line | Col | Main | PR 68 | Span | Cause |
| --- | --- | --- | --- | --- | --- |
| 5 | 3 | E1103 | E1103 | `examples` block on `greeting` | Spec |
| 14 | 11 | E1103 | cleared | `print` | Native call. `Console.print` |

After the `examples` block is deleted, the pull request checker reports E1103 on `name` inside `"Hello, #{name or "world"}!"`. `binary` returns unfinished for `or` when the left side is `Option`. No listed task adds that operator.

## wc.mo

| Line | Col | Main | PR 68 | Span | Cause |
| --- | --- | --- | --- | --- | --- |
| 13 | 3 | E1103 | E1103 | `examples` block on `count` | Spec |
| 31 | 3 | E1103 | E1103 | `examples` block on `row` | Spec |
| 39 | 3 | E1103 | cleared | `"#{n}"` | Interpolation |
| 45 | 20 | E1103 | cleared | `list` | Native call. `Args.list` |

`count` is one of the two functions whose body is skipped. The body calls `text.chars.count(fn(c) c == '\n' end)`. There is no list method `count` in `prims.def`. The spec probe reports E0055 on that call. The headline is `a call's arguments don't match its parameters`. The dot name binds the module function `count`, which takes `Text`. F6 is the nearest task. The real file does not report E0055. The spec fails first.

`main` takes `Args`, `Console`, and `ReadFiles`, and returns `Int`. The command does not reject that signature.

## find.mo

| Line | Col | Main | PR 68 | Span | Cause |
| --- | --- | --- | --- | --- | --- |
| 29 | 31 | E1103 | cleared | `"unknown option #{o}"` | Interpolation |
| 34 | 3 | E1103 | E1103 | `examples` block on `parse_args` | Spec |
| 69 | 3 | E1103 | E1103 | `examples` block on `matches` | Spec |
| 85 | 25 | E1103 | cleared | `ascii_lowercase` | Native call. `Text.ascii_lowercase` |
| 89 | 24 | E1103 | cleared | `list` | Native call. `Args.list` |
| 102 | 16 | E1103 | cleared | `read_text` | Native call. `ReadFiles.read_text` |

With both `examples` blocks deleted, `mo check` on the pull request binary exits 0 for this file. That run is a probe, not the sample. The sample still exits 1. F1 plus the missing spec gate is what makes find pass the command. F8 still owns the sample, and it cannot finish while the spec gate returns E1103.

## todo.mo

| Line | Col | Main | PR 68 | Span | Cause |
| --- | --- | --- | --- | --- | --- |
| 33 | 25 | E1103 | cleared | `"line #{n} of #{file} isn't an item"` | Interpolation. The next row is the form the pull request reports instead |
| 33 | 41 | none | E1103 | `file` | Constant. `file = "todo.txt"` |
| 40 | 3 | E1103 | E1103 | `examples` block on `parse_command` | Spec |
| 59 | 3 | E1103 | E1103 | `examples` block on `parse_items` | Spec |
| 80 | 3 | E1103 | E1103 | `examples` block on `format` | Spec |
| 93 | 3 | E1103 | E1103 | `promise` block on marking an item done | Spec |
| 120 | 13 | E1103 | E1103 | `try parse_command(args.list).or_fail(Usage)` | Try |

`main` returns `Result(Unit, TodoError)` and takes `Args`, `Console`, and `Files`. The command does not report that signature. See the last section for what `mo_check` would report.

The spec probe on the pull request also reports `try` on `n.to_int`, on `drop_prefix(...).or_fail(...)`, and on `items.get(...).or_fail(...)`, plus E0050 on `List.map`. Those functions are the ones whose real diagnostic is an `examples` block.

## kv.mo

| Line | Col | Main | PR 68 | Span | Cause |
| --- | --- | --- | --- | --- | --- |
| 46 | 18 | E1103 | cleared | `"\"#{text}\""` | Interpolation |
| 56 | 3 | E1103 | E1103 | `examples` block on `parse` | Spec |
| 79 | 11 | E1103 | E1103 | `try seconds.to_int.or_fail(...)` | Try |
| 89 | 3 | E1103 | E1103 | `promise` block on expiry | Spec |
| 121 | 3 | E1103 | E1103 | `examples` block on `increment` | Spec |
| 146 | 3 | E1103 | E1103 | `examples` block on `expire` | Spec |
| 173 | 35 | E1103 | cleared | `remove` | Native call. `Map.remove` |
| 180 | 32 | E1103 | cleared | `set` | Native call. `Map.set` |
| 184 | 15 | E1103 | E1103 | `empty_store` | Constant |
| 200 | 33 | E1103 | cleared | `"unknown command #{name}"` | Interpolation |

`increment` is skipped because of its `examples` block. The body is `store.entries.get(key) or Item(...)`. The pull request probe reports E1103 on that `get` call, which is the left operand of `or` on `Option`. `max_expiry` is the same kind of constant as `empty_store`, and the probe reports it. `main` takes `Console` and `Clock`. The command does not reject that signature.

## account.mo

| Line | Col | Main | PR 68 | Span | Cause |
| --- | --- | --- | --- | --- | --- |
| 34 | 3 | E1103 | E1103 | `promise` block on deposit | Spec |
| 54 | 3 | E1103 | E1103 | `promise` block on withdrawal | Spec |
| 81 | 3 | E1103 | E1103 | `promise` block on transfer | Spec |

F1 changes nothing here. The probe reports E1103 on `max_amount` in the deposit and withdrawal bodies, and E1103 on `try withdraw(from, amount)` in `transfer`. There is no `main`. F2 does not apply. The file has no `use`.

## expenses.mo

| Line | Col | Main | PR 68 | Span | Cause |
| --- | --- | --- | --- | --- | --- |
| 23 | 32 | E1103 | cleared | `"expected 3 fields, found #{n}"` | Interpolation |
| 30 | 3 | E1103 | E1103 | `examples` block on `parse_line` | Spec |
| 50 | 3 | E1103 | E1103 | `examples` block on `totals` | Spec |
| 64 | 3 | E1103 | E1103 | `examples` block on `layout` | Spec |
| 80 | 13 | E1103 | cleared | `list` | Native call. `Args.list` |
| 89 | 21 | E1103 | cleared | `read_text` | Native call. `ReadFiles.read_text` |

The probe reports E0050 on `line.split(",").map(...)`, on `sums.update(...)`, and on `rows.map(...)`. `split` itself is a primitive and is not the failing name. `Map.update` and `List.map` are not in `prims.def`. `totals` calls `sort_by_desc` after `update`, so the probe never reaches `sort_by_desc`. That name is also absent from `prims.def`. F6 is the nearest task for all three.

## inventory.mo

| Line | Col | Main | PR 68 | Span | Cause |
| --- | --- | --- | --- | --- | --- |
| 24 | 3 | E1103 | E1103 | `examples` block on `top_by` | Spec |
| 35 | 3 | E1103 | E1103 | `examples` block on `count_by` | Spec |
| 49 | 3 | E1103 | E1103 | `examples` block on `first_by` | Spec |
| 68 | 3 | E1103 | E1103 | `examples` block on `table` | Spec |
| 81 | 26 | E1103 | cleared | `concat` | Native call. `List.concat` |
| 81 | 38 | none | E0050 | `map` | List.map. `rows.map(column.cell)`, reported once `concat` is accepted |
| 97 | 48 | E1103 | cleared | `to_fixed` | Native call. `Decimal.to_fixed` |
| 103 | 33 | E1103 | cleared | `to_decimal` | Native call. `Int.to_decimal` |
| 107 | 11 | E1103 | cleared | `print` | Native call. `Console.print` |
| 108 | 30 | none | E1103 | `products` | Constant. The list bound above `main` |

`top_by` and `count_by` are generic and carry a `where` clause. The command does not report the `where` clause. It reports the spec, then stops. The probe reports E0050 on `sort_by_desc`, on `update`, and on two `map` calls, then the `products` constant. F6 is the nearest task for the generic functions and the function-taking calls. F1 does not clear them.

## notes-tool.mo

| Line | Col | Main | PR 68 | Span | Cause |
| --- | --- | --- | --- | --- | --- |
| 35 | 3 | E1103 | E1103 | `promise` block on `search_notes` | Spec |

The probe reports E1103 on `max_limit` in `input.limit > max_limit`. `trim` and `ascii_lowercase` in that body are not the failing form. There is no `main` and no `use`.

## json-query.mo

| Line | Col | Main | PR 68 | Span | Cause |
| --- | --- | --- | --- | --- | --- |
| 28 | 25 | E1103 | cleared | `"unexpected character at position #{at}"` | Interpolation |
| 34 | 3 | E1103 | E1103 | `examples` block on `parse` | Spec |
| 52 | 14 | E1103 | cleared | `get` | Native call. `List.get` |
| 58 | 23 | none | E1103 | `try string_at(chars, at + 1, [])` | Try. Reached once `get` is accepted |
| 68 | 17 | E1103 | cleared | `char_count` | Native call. `Text.char_count` |
| 78 | 7 | E1103 | E1103 | `try chars.get(at).or_fail(UnexpectedEnd)` | Try |
| 97 | 3 | E1103 | E1103 | `examples` block on `hex_value` | Spec |
| 112 | 13 | E1103 | cleared | `ascii_lowercase` | Native call. `Char.ascii_lowercase` |
| 136 | 10 | E1103 | cleared | `Text.from_chars(...)` | Native call. `Text.from_chars` |
| 137 | 11 | none | E1103 | `try text.to_decimal.or_fail(...)` | Try. Next form in that function |
| 142 | 14 | E1103 | cleared | `get` | Native call. `List.get` |
| 151 | 46 | E1103 | cleared | `empty?` | Native call. `List.empty?` |
| 152 | 19 | none | E1103 | `try value_at(chars, at)` | Try. Next form after `empty?` |
| 165 | 49 | E1103 | cleared | `empty?` | Native call. `Map.empty?` |
| 166 | 12 | none | E1103 | `try expect(chars, at, '"')` | Try. Next form after `empty?` |
| 182 | 14 | E1103 | cleared | `get` | Native call. `List.get` |
| 190 | 14 | E1103 | cleared | `get` | Native call. `List.get` |
| 198 | 3 | E1103 | E1103 | `examples` block on `pretty` | Spec |
| 223 | 3 | E1103 | E1103 | `examples` block on `quoted` | Spec |
| 241 | 12 | E1103 | cleared | `code` | Native call. `Char.code` |
| 247 | 27 | E1103 | cleared | `chars` | Native call. `Text.chars` |
| 255 | 3 | E1103 | E1103 | `examples` block on `at_path` | Spec |
| 279 | 17 | E1103 | cleared | `read_line` | Native call. `Console.read_line` |
| 286 | 20 | E1103 | cleared | `list` | Native call. `Args.list` |
| 304 | 15 | E1103 | cleared | `print` | Native call. `Console.print` |

The `pretty` and `quoted` bodies are skipped. The probe reports E0050 on `map` in both. `main` returns `Int` and takes `Args` and `Console`. The command does not reject that signature.

## rate-limit.mo

| Line | Col | Main | PR 68 | Span | Cause |
| --- | --- | --- | --- | --- | --- |
| 27 | 3 | E1103 | E1103 | `examples` block on `new_limiter` | Spec |
| 39 | 3 | E1103 | E1103 | `promise` block on `request` | Spec |
| 66 | 14 | E1103 | E1103 | `now` | Time minus. The form is `now - bucket.updated` |
| 73 | 45 | E1103 | cleared | `ceil` | Native call. `Decimal.ceil` |

The `request` body is skipped. The probe reports E1103 on `limiter.buckets.get(client) or full`. The failing form is `or` on `Option`, and the span is the `get` call on the left. `Map.get` is a primitive. There is no `main`.

## job-queue.mo

| Line | Col | Main | PR 68 | Span | Cause |
| --- | --- | --- | --- | --- | --- |
| 39 | 3 | E1103 | E1103 | `promise` block on submit | Spec |
| 55 | 3 | E1103 | E1103 | `promise` block on `lease` | Spec |
| 82 | 3 | E1103 | E1103 | `promise` block on `acknowledge` | Spec |
| 100 | 3 | E1103 | E1103 | `promise` block on `fail` | Spec |
| 120 | 3 | E1103 | E1103 | `examples` block on `expire` | Spec |
| 135 | 9 | E1103 | E1103 | `try queue.jobs.get(id).or_fail(...)` | Try |
| 144 | 39 | E1103 | E1103 | `max_attempts` | Constant |
| 150 | 27 | E1103 | cleared | `set` | Native call. `Map.set` |
| 157 | 20 | E1103 | E0050 | `map` | List.map. `jobs.map(fn(job) (job.id, job) end)` |
| 169 | 70 | E1103 | E1103 | `t0` | Constant. Used from `tried` |

The `lease` body is skipped. The probe reports `try now.checked_add(length).or_fail(...)` there. `checked_add` is a primitive. `try` is the unfinished form. There is no `main`.

## What mo run needs

Every sample, on both compilers, prints one line and exits 1.

```text
error E1400: this build cannot run programs yet
```

R1 is the task that removes that refusal. The comment above the inference loop says the command should call `mo_check`, then lower, then code generation, then the image. Inference results are not passed to lowering.

R2 is the task that supplies capabilities at entry. Eight samples define `main`.

| Sample | main parameters | Result written on main |
| --- | --- | --- |
| greet.mo | `Args`, `Console` | none, so `Unit` |
| wc.mo | `Args`, `Console`, `ReadFiles` | `Int` |
| find.mo | `Args`, `Console`, `ReadFiles` | `Int` |
| expenses.mo | `Args`, `Console`, `ReadFiles` | `Int` |
| json-query.mo | `Args`, `Console` | `Int` |
| todo.mo | `Args`, `Console`, `Files` | `Result(Unit, TodoError)` |
| kv.mo | `Console`, `Clock` | none, so `Unit` |
| inventory.mo | `Console` | none, so `Unit` |

account.mo, job-queue.mo, notes-tool.mo, and rate-limit.mo define no `main`. They are modules. `mo run` has nothing to enter until some other file calls them, or until a `main` is added. R2 does not by itself give those four a program entry.

R3 writes bytecode. Nothing in these diagnostics asks for `mo emit`.

R4 is closures and Map code generation. This audit did not run code generation, because `mo run` never gets there. The samples that use `List.map`, `Map.update`, or a function value are expenses, inventory, todo, json-query, and job-queue. Whether those forms fail in code generation is not measured here.

M1 runs `mo test`. The command ignores `test` blocks. greet's test is not among the diagnostics. M1 does not make `mo check` exit 0.

## Gates the split does not name

**The checker refuses an `examples` or `promise` block.** All 12 files fail on at least one. 36 of the 81 main diagnostics are this gate, and it hides the body. F7 to F17 cannot meet their acceptance line until the checker accepts the block. M1 runs examples under `mo test`. That command is a later step. It does not make `mo check` accept the block.

**`try` and `or_fail`.** The command reports `try` on todo, kv, json-query, and job-queue. The spec probe also finds it on account. F5 removes the E1103 for main returning `Result`. That is a different form.

**`or` on `Option`.** The real files hide it behind a spec. The spec probe reports it. greet uses `name or "world"`. kv uses `get(key) or Item(...)`. rate-limit uses `get(client) or full`. No F2 to F6 task names this operator.

**Same-module constants.** The command reports `empty_store`, `max_attempts`, `t0`, `file`, and `products`. The probe also reports `max_amount`, `max_expiry`, and `max_limit`. F3 says constants and initializers across modules. A cross-module fixture can pass and leave every one of these bindings unfinished.

**Arithmetic on `Time`.** rate-limit.mo is the reported case, `now - bucket.updated`. M5 can add a Time module later. The refusal is in `binary`, before any library body runs.

**Function-taking collection calls that are not primitives.** `List.map`, `Map.update`, and `List.sort_by_desc` are absent from `prims.def`. wc's `chars.count(fn ...)` is the same hole. The spec probe reports it as E0055, because the dot name binds the module function `count`. F6 is the nearest task. Its text does not name these four calls. One fixture for a closure does not cover them.

**`mo check` does not call `mo_check`.** F4 and F5 describe E1103 from `src/check/program.c`. The command never prints those messages. On main, that file returns E1103 when `main` has more than one parameter, and when its only parameter is not `Console`. The headline is `this main capability convention is not implemented`. A `main` that returns `Result` returns E1103. The headline is `this main result convention is not implemented`. The same file returns E1103 for a `use` import, a constant declaration, a `test` declaration, a generic function, a `where` clause, and a function named `to_text` or `compare`. It returns E0079 when the entry module has no `main`. The headline is `the entry module has no main function`. Pull request 68's `src/check/program.c` drops the early return for a generic function, a `where` clause, and a function named `to_text` or `compare`. It still returns E1103 for a `use` import, a constant, a `test`, the main capability convention, and a `Result` return. It still returns E0079 when `main` is missing. The command prints none of those until R1 points it at `mo_check`. Until then, `mo check` on greet points at the `examples` block. F4's text does not mention that block.

**These samples do not need F2.** None of the 12 files contains `use`. Cross-module generic calls are not why `mo check` fails here.

**Four samples have no `main`.** account, job-queue, notes-tool, and rate-limit. The command does not require `main`. Certified `mo_check` returns E0079 for a module with no `main`. If R1 calls `mo_check` unchanged, those four gain an E0079 they do not have today. F7 to F17 say `mo check` exits 0. That sentence matches the command. Certified `mo_check` still requires `main`.
