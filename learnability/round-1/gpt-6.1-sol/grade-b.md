# Grade B: gpt-6.1-sol, round 1

Grader B, a fresh session. Judged against `docs/reference/README.md` (v0.1 draft,
11 Oct 2026) alone, following `learnability/rubric.md`. "Parses" is given by the
executable grammar for all twelve. Line numbers are those of the answer files.

Checked for every program: library names and signatures (14); types (`Option`
and `Result` never used as plain values, no `Int`/`Decimal`/`Float`/`BigInt`
mixing, comparisons of one type); failures passed on or handled and no non-`Unit`
result dropped (8.3, 6.1); no rebinding of a visible name, nothing unused (6.1);
assignment only to a `var` or its fields (6.2); `case` coverage and no whole-arm
catch-all over a choice type (7.2); guards only on `return`/`break`/`continue`
(5.1); one-line `if` always with `else` (7.1); capabilities only from parameters,
fakes only in tests/examples/promises (11, 12.4); promise sentence, body, and
`allows`/`forbids` anchors naming every parameter and `result`, with each anchor
checked against the body by hand (12.2); module name matching the file name (2.1).

## 1. greet (`greet.mo`)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes. `args.list.first or "world"` is `Text` (3.7, 8.1); `main(args: Args, console: Console)` with no result exits 0 (11.2). | yes: prints `Hello, NAME!` for the first argument, `Hello, world!` without one. | **pass** |

Stumbles: none.

## 2. wc (`wc.mo`)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes. `files.read_bytes` is on `ReadFiles` (14); `data.size`, `data.get(i) : Option(Byte)`, `value.to_int` exist; the `None` arm is a `stop` (8.4); `totals.lines += ...` assigns a `var`'s fields (6.2); `Counts` fields `lines`/`words`/`bytes` may share library names (5.4, 10.5); `case files.read_bytes(name)` covers `Ok`/`Error` and is used as a statement whose arms end in assignments (7.2); `main : Int` returns `status` (11.2). | yes: per file prints `lines words bytes name`, counting LF bytes, ASCII-white-space-separated words (bytes 9–13 and 32) and raw bytes; a `total` line when more than one file; an unreadable file is reported on stderr and the status is 1. No arguments gives a usage message and status 1 (the task leaves it open). | **pass** |

Stumbles: none.

## 3. find (`find.mo`)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes. `options : Option(Options)` uses `try` on an `Option` in an `Option` function (8.3) and `arguments.drop(n)` (14); `settings = case ... None:` ends in `return 2` (8.4); `var count = 0` inside the `Ok` arm of a loop is a fresh block each round (6.1) and `count` may share a library name (10.5); `for (index, line) in text.lines.enumerate` destructures (7.5, 14); `matched?`/`failed?` are `var`s assigned from nested blocks of the same function (6.2); the body ends in a chained one-line `if` giving the `Int` status (7.1). | yes: `path:number:line` per match, `path:count` with `--count`/`-c`, ASCII case folded with `--ignore-case`/`-i`, `--` ends options; status 2 on an unknown option, a missing phrase or file, or an unreadable file, else 0 when any line matched, else 1. | **pass** |

Stumbles: none.

## 4. expenses (`expenses.mo`)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes. `var fields = []` / `var totals = {}` get their types from their uses (5.1); `chars.get(index + 1) == Some('"')` compares two `Option(Char)` (9.2); `case pieces` on a `List` may end in `_` since a list isn't a choice type (7.2), and the guard arm `[integer, digits] if ...:` continues on the next line (7.2, grammar `arm`); `(whole, fraction) = case ...` is a tuple target (3.6); `amount * 10`, `amount < 0`, `magnitude / 100` give the literal the `BigInt` type its context needs (3.3) and `-amount`, `-1`-style negation is BigInt arithmetic (3.2, 14); `fn(ch)` parameters at lines 58–59 and the loop's `ch` at line 61 are never visible together (6.1); `try` on `Option` in `expense : Option(...)` (8.3); `sort_by_desc(fn(entry) entry.1 end)` keys on a `BigInt` with `Order` (14). | yes: skips the header, parses `date,category,amount` (quoted CSV fields too), amounts with up to two decimals, totals per category in exact cents and prints `category,amount` largest first; a malformed line is reported as `path:N: malformed expense` with its 1-based line number (`index + 1` over `lines.enumerate`) and skipped, status then 1. | **pass** |

Stumbles: none.

## 5. json (`json.mo`)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes. `while ... is Some(ch) and (...)` binds `ch` after `and` (7.4); `try x.or_fail(Syntax(...))` in `Result(_, JsonError)` functions (8.3); `case escaped`/`case ch` over a `Char` may end in `_`/`other` (7.2); `'\u{8}'`, `'\u{C}'`, `'\\'` are listed escapes (3.4); `Some(_) | None` lists both cases rather than a catch-all (7.2); `while true ... end` followed by `stop` as the function's last line (8.4); `(value, after_value) = try parse_value(next)` destructures each round (3.6, 6.1); `rows` bound in two separate arms (6.1); `entries.entries` finds `Map.entries` first (5.4); `current = case current ... end` assigns a `var` from a `case` (6.2, 7.2); `Decimal.to_text`, `Text.from_chars`, `List.take/drop`, `Option.or_fail` as in 14. The `select`, `pretty` and `parse_value` cases cover every case of `Value`/`Option(Char)`. | yes: reads all of stdin, parses objects, arrays, strings with the standard escapes, numbers (as `Decimal`), `true`/`false`/`null`, rejects trailing input; prints with two-space indentation, `[]`/`{}` for empties, keys quoted and escaped; with one argument such as `items.0.name` prints only that value; errors go to stderr with status 1. Exponents and `\u` escapes are declined, as allowed. | **pass** |

Stumbles: none.

## 6. todo (`todo.mo`)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes. `fn to_text(problem: TodoError)` customizes the `Text` ability (9.2), so `main : Result(Unit, TodoError)` prints it and exits 1 on `Error` (11.2); `Error(NotFound(path: _))` then `Error(problem)` covers `FileError` with named-field and name patterns, not a whole-arm catch-all (7.2, 7.3, 10.5); the two `is Some(body)` bindings live in separate `if`/`else if` blocks (6.1, 7.4); `try files.write_text(...)` in `save : Result(Unit, TodoError)` wraps `FileError` through the one case `Storage(FileError)` (8.3); `case args.list` on a `List(Text)` with `["add", ..words]` and a final `_` (7.2, 7.3); `var item = ...; item.done? = true` assigns a `var`'s field (6.2); `items.set`/`remove_at : Option(List)` turned into `Result` with `or_fail` (14, 8.1); `load` uses a local `index` but never calls the module's `index` (6.1). | yes: `todo add TEXT...` appends `[ ] TEXT` to `todo.txt` (words joined by spaces), `todo list` prints `N. [ ] text` numbered from 1, `todo done N` marks `[x]`, `todo remove N` deletes; a missing file is an empty list; bad usage or a bad `N` is an error on stderr with status 1. | **pass** |

Stumbles: none.

## 7. account (`account.mo`)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes. Module `Account` may declare type `Account` (10.3); `expose Account` hides its fields while `AccountError(..)` shows its cases (10.2); each promise has a sentence, a `Bool` body (a multi-line `case result` over `Ok`/`Error(InvalidAmount)`/`Error(InsufficientFunds)`, complete) and `allows`/`forbids` anchors naming every parameter and `result` (12.2); checked by hand, every `allows` is accepted and every `forbids` rejected by its body; `result.cents == 0`, `amount <= 0`, `account.cents + amount` type the literal as `BigInt` (3.3) and `-1.to_big` is `-(1.to_big)` (Appendix B levels 10–11); promise-arm names `updated`/`debited`/`credited` are out of scope before the body binds them (6.1); `try withdraw(...)`/`try deposit(...)` share the failure type (8.3); examples are `Bool` lines (12.1) and the spec is followed by a blank line (12). | yes: `open`, `deposit`, `withdraw` (refuses `amount <= 0` and `amount > balance`, so it never overdraws), `transfer` built from both; `owner` and `balance` are the only way to read a hidden account; the `transfer` promise states `debited.cents + credited.cents == source.cents + destination.cents`, that money is neither made nor lost. | **pass** |

Stumbles: none.

## 8. rate-limit (`rate-limit.mo`)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes. The constant `epoch = Time.parse(...) or stop "..."` has a decided type and may stop before `main` (10.4, 8.4); `rate: 1.0`, `bucket.tokens >= 0`, `capped >= 1`, `(1 - capped) / limiter.rate`, `calculated >= 0.000001` type their literals as `Float` from context (3.3); `(now - bucket.updated).in_seconds.to_float * limiter.rate` is `Duration.in_seconds : Decimal` then `Decimal.to_float` (14); `limiter.buckets.get(client) is Some(bucket) and now < bucket.updated` binds after `and` (7.4); the nested `case decision` inside the body line covers `Accepted`/`Refused` (7.2); `Float` has `Equal` and `Order` (not `Hash`), so `Limiter`/`Bucket` compare with `==` in the tests and `Float` is never a map key (9.2); `fn(bucket) ... limiter.capacity ... end` captures a value, not a capability (5.6); anchors name `capacity`, `rate`, `result` and `limiter`, `client`, `now`, `result`, and each `allows` is accepted, each `forbids` rejected by its body (12.2); `assert ... is Ok((first, Accepted))` keeps `first` in scope (12.3). | yes: one bucket per client (new clients start full), refill `rate` per second from the passed-in time, capped at `capacity`; a request takes a token or is `Refused(wait_seconds:)` with `(1 - tokens) / rate`; a time before the bucket's last update is an error. Promises on `create` and `request`, two tests; the tests' arithmetic (0.5, 0.25, 1.0) is exact in binary floating point. | **pass** |

Stumbles: none.

## 9. job-queue (`job-queue.mo`)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes. Type `Lease` and function `lease` are different names (2.3); field `jobs` and function `jobs` coexist, `queue.jobs` being the field inside the module (5.4); constants `epoch` and `sample_queue` have decided types (10.4); `fn empty()` is called `empty()` and doesn't clash with `Set.empty`, which lives in Set's module (5.2, 10.5); `Leased(token: _, until: deadline)` is a named-field pattern (7.3) and `Ready | Completed | Dead` completes the `JobState` cases (7.2); `if now >= deadline: (if ... else: Ready) else: job.state` nests a one-line `if` in parentheses (7.1, Appendix B); `updated.next_id += 1` and `leased_job.attempts += 1` add to `var` fields of `BigInt`/`Int` (6.2); `updated.jobs = case updated.jobs.set(index, leased_job) ... None: stop ...` (8.4, 14); `duration <= 0.seconds`, `now.checked_add(duration) : Option(Time)` (3.9, 14); `fn(job) ... now ... end` captures a `Time`, not a capability (5.6); every exposed operation's promise names all parameters and `result`, and its `allows`/`forbids` were checked against the body by hand (12.2); `finish` has no promise and needs none. | yes: `submit` appends a `Ready` job with the next id; `lease` expires first, then leases the first `Ready` job (insertion order, so the oldest) until `now + duration`, counting an attempt and issuing a token; `acknowledge` completes and `fail` re-readies or kills on a matching token, else `StaleLease`/`UnknownJob`; `expire` re-readies or kills past-deadline leases; a job dies after its third attempt fails or expires. The queue is a plain value, and every promise includes `keeps_jobs?`, so no operation loses a job. Two tests trace both paths correctly. | **pass** |

Stumbles: none.

## 10. kv (`kv.mo`)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes. Two `to_text` functions share a name because their first parameters are `CommandError` and `Reply`, both declared here (5.1, 9.2); the module's cases are all distinct and reuse no always-in-scope name (4.2); `var quote = None` takes `Option(Char)` from its later `Some(ch)` (5.1); `quote is Some(delimiter)` binds in the guarded block (7.4); `first = try parts.first.or_fail(...)` then `first.ascii_uppercase` resolves `Text`'s function before the local (5.4, 10.5); `["DEL", first_key, ..remaining]` and a final `_` over a `List(Text)` (7.2, 7.3); `seconds.seconds` calls `Int.seconds` on the local `seconds` (5.4); `updated.entries.get(key).map(fn(entry) entry.value end)` is `Option.map` (14); every `Some(value): value` arm with a `None: return (...)` arm sits in its own block (6.1, 8.4); `case command` covers all five commands and is the function's value; `now.checked_add(seconds.seconds)` guarded against `Duration` overflow (3.9, 13.2); `Store == Store` compares maps of `Entry` with `Option(Time)` (9.2); `main(console: Console, clock: Clock)` loops on `console.read_line is Some(line)` (7.4, 11.2). | yes: parses `SET k v`, `GET k`, `DEL k...`, `INCR k`, `EXPIRE k s` (quoted arguments allowed, command name case-folded) into typed commands, applies them at a given time with lazy expiry, returns `(Reply, Store)`; `INCR` on a non-integer gives `NotInteger`, on `9223372036854775807` gives `IncrementOverflow` via `checked_add` and leaves the store unchanged; `main` serves lines from stdin, printing each reply. The test traces all of this correctly. | **pass** |

Stumbles: none.

## 11. notes-tool (`notes-tool.mo`)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes. `notes.list(".")`, `notes.read_text(name)` are on `ReadFiles` and `"."` is its own folder (14); `names = case ... Error(problem): return Error(FolderRead(problem))` (8.4); `var hits = []` typed from `hits.append(Hit(...))` (5.1); `continue if not name.ascii_lowercase.ends_with?(".md")` is a guard on `continue` (5.1); every `Hit`/`SearchOutput`/`SearchInput` is built with all its fields (4.1); the test's `Files.in_memory({...}).read_only` is a fake inside a `test` (12.4) and `==` on `Result(SearchOutput, SearchError)` works since `FileError`'s fields are `Text` (9.2). | yes: typed input (`query`, `limit` 1..100, else `InvalidLimit`; empty query is `EmptyQuery`), searches each `.md` file of the read-only folder (sorted names, line order, ASCII case folded), returns typed hits `(file, line, text)` and `truncated?` set exactly when another match exists past the limit, or a typed error for folder or file read failures. The test checks limit-reached-not-truncated, truncated, and the bad limit, all correctly traced. | **pass** |

Stumbles: none.

## 12. inventory (`inventory.mo`)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes. Header order `module`, `expose`, `use Unicode16` (10.1) and `Unicode16.display_width(text) : Int` called through the module (14, 10.3); the import is used (6.1); `top_n(items: List(T), count: Int, key: fn(T) : K) : List(T) where K: Order` matches `sort_by_desc`'s requirement (5.5, 14) and `take(count.max(0))` never passes a negative (13.2); `group_by` needs `K: Hash`, `unique` needs `T: Hash` (14) and `Product`'s fields `Text`, `Text`, `Decimal`, `Int` all have `Hash`, only `Float` lacking it (9.2); `all_rows.map(fn(row) row.size end).max or 0` is `Option(Int)` then `or` (14, 8.1); `fn(_) 0 end` names an ignored parameter (2.3); `widths = case widths.set(index, width) ... None: stop ...` (8.4); the anonymous parameters `row` (line 39), `width` (line 52) and the loop names `row`, `width` are never visible together (6.1); `product.price.to_fixed(2)`, `product.stock.to_text` (14); printing happens in a `for`, not in `map` (5.6). | yes: generic `top_n`, `group_items`, `deduplicate`; `table` pads cells to the widest display width per column, two spaces between columns, a dashed rule, trailing space trimmed; `main` deduplicates sample products, prints the table, the top two by price and the groups by category. The test's expected table `"A   B\n--  -\nxx  z\nq"` matches a hand trace (widths 2 and 1; the short row's missing cell is `""`). | **pass** |

Stumbles: none.

## Summary

| # | Program | Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- | --- | --- |
| 1 | greet | yes | yes | yes | pass |
| 2 | wc | yes | yes | yes | pass |
| 3 | find | yes | yes | yes | pass |
| 4 | expenses | yes | yes | yes | pass |
| 5 | json | yes | yes | yes | pass |
| 6 | todo | yes | yes | yes | pass |
| 7 | account | yes | yes | yes | pass |
| 8 | rate-limit | yes | yes | yes | pass |
| 9 | job-queue | yes | yes | yes | pass |
| 10 | kv | yes | yes | yes | pass |
| 11 | notes-tool | yes | yes | yes | pass |
| 12 | inventory | yes | yes | yes | pass |

**Pass: 12. Close: 0. Fail: 0.**

Stumbles for the diagnostic log: none. No form in the twelve files breaks a rule
of the reference as I read it. Forms I examined closely and accepted, each
resting on a sentence of the reference rather than on a guess, so a third grader
can check them: bare literals typed as `BigInt` or `Float` by their context
(`amount * 10`, `result.cents == 0`, `rate: 1.0`; 3.3); `-1.to_big` as
`-(1.to_big)` (Appendix B precedence); `fn(_) 0 end` (2.3, parses); `case`
used as a statement whose arms end in assignments (7.2, 8.1's own example,
grammar `arm`); `while true ... end` followed by `stop` (8.4); a module constant
`Time.parse(...) or stop "..."` (10.4); a multi-line `case result` as a promise
body line (12.2, 2.4); `use Unicode16` (14); struct fields and functions sharing
library names such as `lines`, `jobs`, `chars`, `first`, `seconds` (5.4, 10.5);
the same name bound in separate arms or blocks of one function (6.1, last
sentence); `Decimal` having `Hash` (9.2 excludes only `Float`).
