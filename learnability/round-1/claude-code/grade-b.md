# Grade B: Claude Code, round 1

Grader B, a fresh session. Judged against `docs/reference/README.md` alone, per
`learnability/rubric.md`. "Parses" comes from the executable grammar: yes for
all twelve. Line numbers refer to the file in `claude-code/`.

## 1. greet

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes found. `main` returns `Unit`, so the tests may drop its result (6.1); `join`, `trim`, `empty?` are called as section 14 gives them; fakes appear only in tests (12.4). | yes: joins all arguments with spaces, trims, prints `Hello, world!` when nothing or only blanks are given; exit 0. | pass |

Stumbles: none.

## 2. wc

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes found. `read_bytes(name).map(tally)` passes a named function as a value (5.6); the `tally` promise names `data` and `result` in every anchor, and its body accepts the `allows` and rejects all three `forbids` (12.2); each `case` covers `Ok`/`Error` and `Some`/`None`; re-used names (`name`, `counted`, `counts`) are in separate loops or arms (6.1); fakes only in examples and tests. | yes: counts lines, words and bytes per file, right-aligned like wc; prints a total only for more than one file; an unreadable file is reported on standard error, the others still counted, status 1; falls back to standard input with no file (an extra, not asked). The expected values in the examples and tests check out. | pass |

Stumbles: none.

## 3. find

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes found. Names bound by `rest is [word, ..others]` are used in the `while` block and after `and` (7.4); `case rest` with `[]`, `[_]`, `[phrase, ..paths]` covers every list, and the `_` arms are over `Text` and `Char`, not choice types (7.2); `to_text(problem: UsageError)` is the Text customization of 9.2; the anonymous functions capture only `needle` and `ignore_case?` (11.4); every `Result` is handled with `case`. | yes: `--ignore-case`/`-i`, `--count`/`-c`, combined short options and `--`; prints `FILE:NUMBER:LINE` or `FILE:COUNT`; exit 0 on a match, 1 on none, 2 on a usage error (with usage on standard error) or an unreadable file, while the other files are still searched. All eight tests' expected values check out. | pass |

Stumbles: none.

## 4. expenses

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes found. The `_:` and `Error(failure):` arms in `main` end in `return 2`, so they need no value (8.4) and are over a list and a `Result`, not a catch-all over a choice type; `try amount.to_decimal.or_fail(NotANumber(...))` matches 8.3 with the same failure type; `amount: 800` takes `Decimal` from its field (3.3); `group_by`, `sum`, `sort_by`, `sort_by_desc`, `max or 0`, `to_fixed`, `pad_left`/`pad_right` all as section 14 gives them; `to_text(problem: LineError)` is the 9.2 customization. | yes: skips the header, totals by category, largest first with ties by category (stable sorts, 13.3), prints aligned columns to the cent; a malformed line is reported on standard error as `line N: ...` with the right number (`index + 1` after `enumerate.drop(1)`) and skipped, status 1; a missing or unreadable file gives status 2. The examples and tests check out, including `1.005` caught by `round(2) != value`. | pass |

Stumbles: none.

## 5. json

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes found. `document = try parse(...)` in `main : Result(Unit, Problem)` wraps the `SyntaxError` because `BadJson(SyntaxError)` is `Problem`'s one case holding it (8.3); `try key.to_int` and `try child_at(...)` are in `Option`-returning functions (8.3); every `case` over `Option(Char)` or `Json` lists its cases, using `Some(_) | None` and `Array([])`/`Array(items)` rather than a whole-arm catch-all (7.2), and the `_` arms are over `Char`; `inner`/`out` are re-bound only in separate arms (6.1); the case names `Null`, `Boolean`, `Number`, `String`, `Array`, `Object` clash with nothing always in scope (4.2, 9.2 keeps ability names after `where`); `lines`, `error`, `next` are legal locals (10.5, 2.3). | yes: reads all of standard input, parses objects, arrays, strings with escapes, numbers (keeping their spelling, exponents included), `true`/`false`/`null`; lays the value out with two-space indentation and `"key": value` members; with a path like `items.0.name` prints only that value; a bad path or invalid JSON is an `Error`, printed with line and column, exit 1. The tests' expected layouts and the error position (line 2, column 7) check out. | pass |

Stumbles: none.

## 6. todo

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes found. `try files.write_text(...)` in `save : Result(Unit, TodoError)` wraps the `FileError` through `FileTrouble(FileError)`, the one case holding it (8.3); `try save(...)` on a line of its own drops only `Unit` (6.1); `case words` over a `List(Text)` ends in `_`, which 7.2 allows for a non-choice type; `Error(NotFound(path: _))` before `Error(problem)` covers `Result` fully; both promises give every parameter and `result` in each anchor, and `result is Ok(number) implies number >= 1` uses the binding after `implies` (7.4); `files.read_only` narrows the capability for `load` (11.3); fakes only in tests. | yes: `add` joins the words and appends, `list` numbers from 1 (padded past 9 items), `done N` marks `[x]`, `remove N` deletes; a missing file counts as an empty list; bad numbers, missing items and empty text give status 1 with a message, anything else prints usage and gives 2. The tests' expected output and file contents check out. | pass |

Stumbles: none.

## 7. account

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes found. `expose Account, AccountError(..), ...` hides the record's fields (10.2); `fn open(owner: Text)` beside an `owner` function is 6.1's own permitted example, and `open` never calls `owner(...)`; `result.owner`, `account.balance` are field reads where the fields are visible (5.4); all eight promises have a sentence, a `Bool` body and `allows`/`forbids` anchors giving every parameter and `result`, with multi-line anchors continued by trailing commas (2.4), and each body accepts its `allows` and rejects its `forbids` (12.2); `is Ok(added) implies added == ...` uses 7.4's binding; `try account.balance.checked_add(amount).or_fail(BalanceTooLarge)` is 8.3's form; `total : BigInt` compares `BigInt` with `BigInt`. | yes: `open`, `deposit`, `withdraw` (refused with `InsufficientFunds` when the balance can't cover it, so it never overdraws), `transfer` built from the two with all-or-nothing semantics on immutable values; promises include "a transfer neither makes nor loses money" via `total`; examples on every function. The examples' and tests' expected values check out, including the `Int` overflow cases. | pass |

Stumbles: none.

## 8. rate-limit

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes found. Parameters named `limit` and `limiter` sit beside functions of those names, but no function with such a parameter calls the function, which 6.1 allows (its `open`/`owner` example), and `limiter.limit` is a field read (5.4); all nine promises have a sentence, `Bool` lines and `allows`/`forbids` anchors naming every parameter and `result`, with each body accepting its `allows` and rejecting its `forbids` (checked by hand, e.g. 750 ms accepted, 700 and 800 ms rejected); nested `implies` are parenthesized, so none chain (Appendix B); the constant `epoch = case Time.parse(...) ... end` has a value that decides its type and may `stop` (10.4); `Time`/`Duration` arithmetic and comparisons follow 3.9 and 14; `entries ... .to_map`, `in_seconds`, `ceil`, `floor.to_int`, `checked_add` all as section 14 gives them; `Limiter` compares with `==` since `Time`, `Decimal` and maps have `Equal` (9.2). | yes: one `Bucket` per client in a `Map`, capacity and `per_second` refill as exact decimals, `request` takes a token or returns `Refused(retry_after)` rounded up to the millisecond, time passed in as `now`; promises and thirteen tests, whose expected decisions (1 s, 1 ms, 334 ms, 3 s, 6 s, pruning) all check out by hand. | pass |

Stumbles: none.

## 9. job-queue

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes found. `payload`, `until`, `attempt`, `job_id`, `jobs` and `find` are functions beside fields or List functions of the same name: `held.payload` is the field where it is visible, `queue.jobs.find(ready?)` is List's `find` and `after.find(id)` this module's, by the type of the receiver (5.4), and `submit(queue, payload: Text)` never calls `payload(...)` (6.1); `var job = try held_job(...)` then `job.state = Done` is 6.2's field assignment; `back.state = if ...: Ready else: Dead` assigns a one-line `if`; `{id: Job(...)}` uses the key expression (3.8); `JobId` is a map key with `Hash` (9.2); `for round in 1..max_tries + 1` binds `1..(max_tries + 1)` (Appendix B); the five promises each have a sentence, `Bool` lines, and `allows`/`forbids` anchors giving every parameter and `result`, and each body accepts its `allows` and rejects its `forbids` (checked by hand); `noon = case Time.parse(...) ... end` as in 10.4. | yes: `submit`, `lease` (oldest ready job, `now` and a `Duration`, refusing a non-positive or over-range length), `acknowledge`, `fail`, `expire`; a job's third failed or expired try makes it `Dead`; the queue is a hidden immutable value; every operation's promise says "no job is lost" and checks `work(after) == work(queue)`. The eight tests' expectations check out (a retried job keeps its place; `expire` at exactly the deadline takes the lease back). | pass |

Stumbles: none.

## 10. kv

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes found. Two `to_text` functions share a name because their first parameters, `Reply` and `Fault`, are different types declared in this module (5.1); `case (upper, args)` matches tuple patterns with literals and list patterns, and its `_` arm is over a tuple, not a choice type (7.2, 7.3); `live` ends with `Some(_) | None` after a guarded `Some(entry) if ...` arm; `store.put(...)`, `store.live(...)`, `entry.alive?(now)` resolve to this module's functions by the receiver's type (5.4); `n = case ... None: return (...)` ends an arm in `return` (8.4); `reduce`, `unique`, `count`, `checked_add`, `Map.remove` as section 14 gives them; `main(console: Console, clock: Clock)` returns `Unit`, so the test may drop its result (6.1). | yes: parses SET/GET/DEL/INCR/EXPIRE case-insensitively with quoted words and escapes, `apply(store, command, now)` returns `(Reply, Store)`, INCR gives `Rejected(Overflow)` on `checked_add` failure and keeps the expiry, EXPIRE is lazy with a guard against `Duration`/`Time` overflow, replies print as redis-cli shows them, and `main` serves standard input at `clock.now`. The nine tests' expected replies check out, including the expiry boundary at exactly 10 s. | pass |

Stumbles: none.

## 11. notes-tool

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes found. `use Unicode16` follows the `expose` line (10.1), and section 14 says the `use` line is how a program picks its Unicode version, so `Unicode16.lowercase` and `Unicode16.normalize_nfc` are qualified calls (10.3); `try notes.list(".")` and `try notes.read_text(name)` in a function returning `Result(Output, SearchError)` wrap the `FileError` through `Unreadable(FileError)`, its one case holding it (8.3); the promise has a sentence, a `Bool` body with a multi-line anonymous function capturing only `input` (11.4), and anchors giving `input`, `notes` and `result`, where the `Files.in_memory({}).read_only` fakes are allowed in promises (12.4); the body accepts the `allows` and rejects all three `forbids`; `continue if not ...` and `return ... if ...` are guards on early exits (5.1); `to_text(failure: SearchError)` is the 9.2 customization. | yes: `handle(input: Input, notes: ReadFiles) : Result(Output, SearchError)`; `Input` holds a query and a limit checked to be 1..100, hits carry file, line number from 1 and text in sorted file order, `truncated?` is set only when a further match is found with the limit full, and the errors are typed, including an unreadable note. Matching ignores case beyond ASCII and normalizes. The three tests' expected values check out. | pass |

Stumbles: none.

## 12. inventory

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes found. `struct Column(T)` holds a `cell: fn(T) : Text` field, and `column.cell(row)` calls it (9.1, 5.4); `product_columns()` and `summary_columns()` are functions with signatures rather than constants, as 10.4 asks for a list of anonymous functions; the generic helpers declare `where K: Order` and `where K: Hash` (5.5) and call the `key` parameter as `key(item)`; `var groups = {}` and `var seen = Set.empty` take their types from their uses (5.1); `fn(_letter) 0 end` marks its unused parameter (6.1); `try arguments.first` and `try text.to_int` are in an `Option`-returning function (8.3); `for ((cell, width), align) in cells.zip(widths).zip(aligns)` is a nested binding (Appendix B); `-product.quantity` negates the field read (precedence table); both promises have a sentence, a `Bool` body and anchors giving every parameter and `result`, accepted and rejected as they should be; `Unicode16.display_width` via `use Unicode16` (10.3, 14). | yes: `top_by` (largest first, stable, `take(n.max(0))`), `group_by_key` (insertion order via `update`), `distinct_by` (first of each key kept), and `table` with columns padded by display width, left or right aligned, two spaces apart; used on a product list in `main` for the stock list, two top-N lists and per-category totals, with the N argument validated (status 2 otherwise). The tests' expected table, top lists and totals (58/115/31 units, 1637.50/1434.25/956.50) check out by hand. | pass |

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

**Count: 12 pass, 0 close, 0 fail.**

No stumbles were found in any of the twelve programs, so the diagnostic log
for this family is empty. The forms that needed the closest reading against
the reference, and turned out to be allowed, were: parameters sharing a name
with a module function (`open(owner: Text)`, `limiter(limit: Limit)`,
`request(limiter: Limiter, ...)`, `submit(queue, payload: Text)`), allowed by
6.1 because none of those functions calls the function of that name; module
functions sharing a name with record fields or List functions (`balance`,
`payload`, `until`, `find`, `jobs`), resolved by 5.4 on the receiver's type;
`case` arms ending in `return` used where a value is expected (8.4); `try`
across differing failure types, always with exactly one wrapping case
(`BadJson(SyntaxError)`, `FileTrouble(FileError)`, `Unreadable(FileError)`);
module constants computed by a `case` over `Time.parse(...)` (10.4); and
fake capabilities inside promise anchors (12.4).
