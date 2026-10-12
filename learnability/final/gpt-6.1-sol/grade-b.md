# Grade B: GPT-6.1 Sol, final round

Grader B, a fresh session, grading against `docs/reference/README.md` alone, with `learnability/rubric.md`. "Parses" comes from the executable grammar and is yes for every file. Line numbers refer to the answer files in this folder.

## 1. greet (`greet.mo`)

| | |
| --- | --- |
| Parses | yes |
| Would compile | yes: no mistakes found. `args.list.first or "world"` is an `Option(Text)` with a default (8.1); `print` takes `Text` (14); `main` without a result exits 0 (11.2). |
| Does the task | yes: greets the first argument, or "world" when there is none. |
| Verdict | pass |

Stumbles: none.

## 2. wc (`wc.mo`)

| | |
| --- | --- |
| Parses | yes |
| Would compile | yes: no mistakes found. `files.read_bytes` is on `ReadFiles` (14); `while data.get(position) is Some(byte)` binds `byte` in the loop (7.4); `byte.to_int` (14); `+=` on the `var`'s `BigInt` fields with `0`/`1` typed by their context (3.3, 6.2); the `case` covers `Ok` and `Error` (7.2); `FileError` has a text form (14); `main : Int` returns `status` (11.2). |
| Does the task | yes: per file prints lines (newline bytes), words (runs between ASCII white space, codes 9 to 13 and 32) and bytes, then the file name; prints a `total` line when more than one file is named; an unreadable file is reported on standard error and the exit status is 1. |
| Verdict | pass |

Stumbles: none.

## 3. find (`find.mo`)

| | |
| --- | --- |
| Parses | yes |
| Would compile | yes: no mistakes found. Options are consumed with `while operands.first is Some(option)` and `drop(1)` (7.4, 14); `case operands` uses a list pattern `[phrase, first_file, ..remaining_files]` with a `_` arm, allowed because a list isn't a choice type (7.2, 7.3); `for (index, line) in text.lines.enumerate` (14); `contains?`, `ascii_lowercase`, `prepend` have these signatures (14); `return 2` leaves `main : Int` from an arm (8.4); the last line is a one-line `if` chain giving an `Int` (7.1). |
| Does the task | yes: prints `FILE:LINE:text` for matching lines, `-i`/`--ignore-case` lowercases both sides, `-c`/`--count` prints `FILE:count` per file instead; exit 0 when something matched, 1 when nothing did, 2 on an unknown option, a missing phrase or file, or an unreadable file (reported on standard error). |
| Verdict | pass |

Stumbles: none.

## 4. expenses (`expenses.mo`)

| | |
| --- | --- |
| Parses | yes |
| Would compile | yes: no mistakes found. The CSV state machine lists all four `CsvState` cases and uses `_` only on `Char` values (7.2); `var fields = []` and `var totals = {}` take their types from their uses in the body (5.1); `try` on `Option`s in `Option`-returning functions (8.3); `path = case ...` and `contents = case ...` have arms ending in `return 1`, which fit where a value is expected (8.4); `Some((category, amount))` is a tuple pattern inside a case pattern (7.3); `totals.get(category) or 0` gives a `Decimal` from `previous + amount` (3.3, 8.1); `sort_by_desc(fn(entry) entry.1 end)` over `entries` (14); `to_fixed(2)` (14). |
| Does the task | yes: reads the one file named, skips the header (`enumerate.drop(1)` keeps the physical line number), parses `date,category,amount` (quoted fields handled), reports a malformed line as `FILE: line N: malformed expense` on standard error and skips it with exit status 1, prints `category: total` largest first. |
| Verdict | pass |

Stumbles: none.

## 5. json (`json.mo`)

| | |
| --- | --- |
| Parses | yes |
| Would compile | yes: no mistakes found. `JsonValue` is a recursive choice type (4.2); `fn to_text(problem: JsonError)` customizes its text form, which `main : Result(Unit, JsonError)` prints (9.2, 11.2); the `Input` cursor is a `var` whose `position` field gets `+=` (6.2); `while ... is Some(ch)` and `is Some(next) and next.ascii_digit?` bind in the guarded block (7.4); `try x.or_fail(InvalidJson(...))` and `try parse_value(...)` keep one error type (8.3); `(item, after_item) = try ...` destructures the tuple results (3.6); `case` on a `Char` may end in `_` or a name arm (7.2); `pretty` and `select_path` list all six `JsonValue` cases, with `JsonNull \| JsonBool(_) \| ...` sharing a line (7.2); the anonymous functions read only fixed names and parameters (5.6); `chars`, `code`, `ascii_digit?`, `drop`, `take`, `join`, `entries`, `all?`, `to_int`, `split`, `read_line` have these signatures (14). |
| Does the task | yes: reads all of standard input, parses JSON (numbers kept as validated spellings, strings unescaped, exponents and `\u` escapes rejected with a message, trailing text rejected), prints it with two-space indentation (`[]`/`{}` for empty), or walks a dotted path through objects and array indexes and prints that value; parse and path errors go to standard error with exit status 1. |
| Verdict | pass |

Stumbles: none.

## 6. todo (`todo.mo`)

| | |
| --- | --- |
| Parses | yes |
| Would compile | no, 1 mistake. Line 19, `IoFailure(cause): cause.to_text`: `FileError` has no `to_text` function; section 14 lists `to_text` only on the types it names (`Int`, `Decimal`, `Char`, `Time`, ...), and gives `FileError` just its cases and text forms, which are reached with `"#{cause}"` (3.4, 9.2, 14). Everything else holds: `try files.write_text(...)` in a `Result(Unit, TodoError)` function wraps the `FileError` into the one case `IoFailure(FileError)` (8.3); `Error(NotFound(path: _))` before `Error(problem)` covers `Result` (7.2); `["add", ..words]` and the other list patterns (7.3); `drop_prefix ... or line`, `remove_at`/`set` as `Option(List(T))`, `read_only`, `enumerate`, `join` (14); `updated = if remove? ... else ... end` is a block `if` after `=` (7.1). |
| Does the task | yes: `add TEXT` joins the remaining arguments and appends a `[ ] TEXT` line to `todo.txt` (a missing file counts as empty); `list` prints `N. [ ] text` numbered from 1; `done N` marks the line `[x]`; `remove N` deletes it; bad numbers, missing tasks and wrong usage become errors printed on standard error with exit status 1. |
| Verdict | close |

Stumbles:
- `cause.to_text` on a `FileError`: not a function section 14 gives `FileError`; the text form comes through interpolation, `"#{cause}"` (3.4, 9.2). Generalized from `Int.to_text`/`Time.to_text` to every type with a text form.

## 7. account (`account.mo`)

| | |
| --- | --- |
| Parses | yes |
| Would compile | yes: no mistakes found. `expose Account` without `(..)` hides the record's insides, and the module shares the type's name (10.2, 10.3); `fn open()` is called as `open()` (5.2); every promise has a sentence, `Bool` lines and `allows`/`forbids` anchors giving every parameter and `result`, and each anchor checks against its body as written (12.2); the `case result` bodies list `Ok` and all three `AccountError` cases (7.2); `result is Ok(updated) implies ...` keeps `updated` in scope after `implies` (7.4); `checked_add(amount).or_fail(BalanceLimit)` with `try`, and `try withdraw`/`try deposit` with one failure type (8.3); `to_big` sums avoid `Int` overflow in the bodies (3.2); the test's `assert ... is Ok(name)` bindings stay in scope (12.3). |
| Does the task | yes: `open`, `deposit`, `withdraw` (refuses `amount > balance` and negative amounts), `transfer` returning both accounts or a failure with neither changed; the type's insides are hidden; promises and examples on every function, including "a successful transfer neither makes nor loses money" with anchors. |
| Verdict | pass |

Stumbles: none.

## 8. rate-limit (`rate-limit.mo`)

| | |
| --- | --- |
| Parses | yes |
| Would compile | yes: no mistakes found. `module Answers.RateLimit` matches `answers/rate-limit.mo` (2.1); the constants `token_units = 1.to_big * 10.to_big.pow(24)` and `epoch = Time.parse(...) or stop "..."` have types their values decide, and a `stop` fits where a value is expected (10.4, 8.4); both promises have a sentence, `Bool` lines and `allows`/`forbids` anchors giving every parameter and `result`, and each anchor checks against its body as written (12.2); `capacity < 1 implies result == Error(InvalidCapacity)` parses as intended by the precedence table (App. B); the `case result` bodies cover `Ok` and all three `RateLimitError` cases, with `Error(InvalidCapacity) \| Error(InvalidRate)` sharing a line, and the inner `case verdict` lists both `Decision` cases (7.2); `is Some(old) and now < old.updated` and `result is Ok(configured) and ...` bind in the same condition (7.4); `BigInt.min`, `pow`, `to_fixed(18)`, `Char.code`, `Duration.in_seconds`, `Decimal.to_int`, `Map.remove`/`set`/`get` have these signatures (14); `updated.buckets = ...` assigns a `var`'s field (6.2); tests reassign a `var` in a `for _ in 0..3` loop and bind with `assert ... is Ok(name)` (12.3). |
| Does the task | yes: one bucket per client in a `Map(Text, Bucket)`, a capacity and a `Decimal` refill rate per second, `request(limiter, client, now)` consumes one token or returns `Refused(wait_microseconds: ...)` with the least wait, time is passed in, old states are unchanged; promises on `create` and `request` and eight tests, whose expected waits I checked by hand. |
| Verdict | pass |

Stumbles: none.

## 9. job-queue (`job-queue.mo`)

| | |
| --- | --- |
| Parses | yes |
| Would compile | yes: no mistakes found. `Queue` and `Lease` are exposed without insides and read through functions named like their fields, which is the field where visible and the function elsewhere (5.4, 10.2); the constant `example_time` is a `case` whose `None` arm stops, so its type is `Time` (10.4, 8.4); all five promises have a sentence, `Bool` lines and `allows`/`forbids` anchors giving every parameter and `result`, built from `empty()`, `example_queue(...)`, `example_receipt(...)` and `example_time`, and each anchor checks against its body as written (12.2); every `case` on `Status` or on a `Result` lists its cases, with `Ready \| Acknowledged \| Dead` sharing a line and `_` only inside patterns (7.2); `is Leased(deadline) and deadline <= now` and `is Some(x) and ...` bind in their conditions (7.4); anonymous functions read only their parameters and the enclosing function's parameters (5.6); `+=`, `.status = ...`, `.entries = ...` go to `var`s (6.2); `queue.entries.entries` is the field, then `Map.entries` (5.4, 14); `find`, `filter`, `all?`, `values`, `set`, `checked_add`, `0.seconds` have these signatures (14). |
| Does the task | yes: `submit`, `lease` (the oldest eligible job by submission order, for a `Duration`, after expiring leases), `acknowledge`, `fail`, `expire_leases`; attempts count up on each lease, a job is never leased with three attempts, and a failed or expired third attempt makes it `Dead`; the queue is a value, and every promise says `identities(result.0) == identities(queue)`, so no operation loses a job. |
| Verdict | pass |

Stumbles: none.

## 10. kv (`kv.mo`)

| | |
| --- | --- |
| Parses | yes |
| Would compile | yes: no mistakes found. `case line.words` with `[]` and `[verb, ..args]` covers every list, and `case name` on a `Text` may end in `_` (7.2, 7.3); `try seconds_text.to_int.or_fail(InvalidSeconds(seconds_text))` in a `Result(Command, CommandError)` function (8.3); `checked_mul(1_000_000)` guards the `Duration` before `seconds.seconds`, and `now.checked_add(...)` is an `Option(Time)` (3.9, 14); every `case` on `Command`, `CommandError`, `Reply` and the `Option`s lists its cases, with `Value(Some(text))` and `Value(None)` covering `Value` (7.2); `(reply, next_store) = run_line(...)` then `store = next_store` keeps the `var` from being bound again (6.1, 6.2); `reply.to_text` is this module's `fn to_text(reply: Reply)`, found first in `Reply`'s own module (5.4, 9.2); `Option.map`, `Map.has?`/`remove`/`set`, `Int.to_text`, `clock.now`, `read_line` have these signatures (14). |
| Does the task | yes: parses `SET k v`, `GET k`, `DEL k...`, `INCR k`, `EXPIRE k s` (case-insensitive verbs, arity checked), applies them at a given `Time` after purging expired keys, returns `(Reply, Store)`; `INCR` reports a non-integer value and overflow (`checked_add`); `EXPIRE` answers 1/0 and deletes on `seconds <= 0`; `main` reads lines from standard input and prints each reply with `clock.now`. |
| Verdict | pass |

Stumbles: none.

## 11. notes-tool (`notes-tool.mo`)

| | |
| --- | --- |
| Parses | yes |
| Would compile | yes: no mistakes found. `try notes.list(".")` and `try notes.read_text(name)` return `Result(_, FileError)` and `try` wraps the failure into the one case `FileFailure(FileError)` (8.3, 14); `continue if not (a or b)` is a guarded early exit with the parentheses `not`'s precedence needs (5.1, App. B); `for (index, text) in content.lines.enumerate` (14); `var hits = []` takes `List(Hit)` from `append` (5.1); the only capability is the `notes: ReadFiles` parameter (11). |
| Does the task | yes: typed `SearchInput(query, limit)`, `limit` must be 1 to 100 or `InvalidLimit`, an empty query is `EmptyQuery`; searches the `.md`/`.markdown` files directly in the read-only folder in sorted order, returns `Hit(file, line, text)` for each matching line, one-based, and `truncated?` when more matches exist past the limit; a listing or read failure is the typed `FileFailure`. |
| Verdict | pass |

Stumbles: none.

## 12. inventory (`inventory.mo`)

| | |
| --- | --- |
| Parses | yes |
| Would compile | yes: no mistakes found. `use Unicode16` then `Unicode16.display_width(cell)` is the module-call form sections 10.3 and 14 give (14: "changes only when its `use` line does"), and the import is used (6.1); `top_n` asks `where K: Order` for `sort_by_desc`, `group_items` `where K: Hash` for `group_by`, `deduplicate` `where T: Hash` for `to_set` (5.5, 14); `take(n.max(0))` and `repeat((...).max(0))` keep the arguments at least 0 (13.2, 14); `struct Column(T)` holds `render: fn(T) : Text`, called as `column.render(item)` (5.6, 5.4); the nested anonymous functions read only their parameters and fixed names (5.6); `entry.0.0` reads a nested tuple (3.6); `value_widths.max or 0` is `Option(Int)` with a default (14, 8.1); `Product` has `Hash` because all its fields do (9.2); the multi-line list literals continue on trailing commas (2.4). |
| Does the task | yes: generic top-N by key (largest first, stable), grouping by key (first-appearance order), duplicate removal (first kept); a text table with headings, a separator, display-width padding and per-column right alignment; `main` prints the product table, the top three by price and each category's group, after removing a duplicate product. |
| Verdict | pass |

Stumbles: none.

## 13. roman (`roman.mo`, held out)

| | |
| --- | --- |
| Parses | yes |
| Would compile | yes: no mistakes found. The constant `numerals` is a `List((Int, Text))` its literal decides (10.4); the promise has a sentence, two `Bool` lines and `allows`/`forbids` anchors giving `n` and `result`, each of which checks against the body as written (12.2); `(n >= 1 and n <= 3999) == (result is Some(_))` compares two `Bool`s and `result is Some(numeral) implies from_roman(numeral) == Some(n)` keeps `numeral` in scope after `implies` (7.4); `for (value, symbol) in numerals` and `while remaining.drop_prefix(symbol) is Some(rest)` (7.5, 14); `-=`/`+=` on `var`s (6.2); `try to_roman(total)` on an `Option` in an `Option`-returning function (8.3); `from_roman(argument).map(fn(n) n.to_text end)` gives the `Option(Text)` the other arm gives (14, 7.2). |
| Does the task | yes: `to_roman` for 1 to 3999, `None` outside; `from_roman` reads greedily, then accepts only when `to_roman` of the total spells the same text, so `IV` but not `IIII` (I traced `IIII`, `IL`, `IIV`, `IXIX`, `MMMM`, all `None`); examples on both, the round-trip promise; `main` converts each argument either way, prints `?` for one it can't, exits 1 if any failed. |
| Verdict | pass |

Stumbles: none.

## 14. word-freq (`word-freq.mo`, held out)

| | |
| --- | --- |
| Parses | yes |
| Would compile | yes: no mistakes found. The two-line `if` condition continues on its trailing `or` (2.4); `while data.get(position) is Some(byte)` and `byte.to_int` (7.4, 14); `code.to_char` is an `Option(Char)` whose `None` arm stops with a reason (14, 8.4); `word` is bound in two separate blocks, the loop's `else if` branch and the final `if` (6.1); `case argument.to_int` with `Some(n) if n >= 0`, then `Some(_) \| None`, covers every value (7.2); `sort_by(fn(entry) (-entry.1, entry.0) end)` keys by a tuple, which has `Order` part by part (9.2, 14); `take(top)` with `top` kept at least 0 (13.2); `var counts = {}` is the `Map(Text, Int)` `count_words` takes (5.1). |
| Does the task | yes: words are runs of ASCII letters and apostrophes read from the files' bytes, lowercased; counts are merged across files; prints the top N as `count word`, most frequent first and ties in alphabetical order; N is 10 or `--top N` (a bad or missing N is a usage error, exit 1); an unreadable file is reported on standard error and the exit status is 1. |
| Verdict | pass |

Stumbles: none.

## 15. library (`library.mo`, held out)

| | |
| --- | --- |
| Parses | yes |
| Would compile | no, 1 mistake. Line 175, `anchor_time - 1.seconds`: a `Time` minus a `Duration`. Section 3.9 gives `Time - Time` (a `Duration`) and `Time + Duration` (a `Time`), and section 14 gives `Time` only `checked_add` besides "the operators of section 3.9"; combining values of different types otherwise is an error (3). Everything else holds: the module shares its type's name and `expose Library` hides the insides (10.3, 10.2); both promises have a sentence, `Bool` lines and `allows`/`forbids` anchors giving `library`, `member_id`, `copy_id`, `now` and `result`, built from the constants `anchor_two`/`anchor_three`, and each anchor checks against its body as written (12.2); `result is Ok(single) implies (...)` and `result is Ok(limited) implies limited.members.to_list.all?(fn(id) ... end)` keep the bound name in scope, and the anonymous function reads only that fixed name (7.4, 5.6); the constants `anchor_time` (a `case` whose `None` arm stops) and the anchor libraries have types their values decide, with no cycle (10.4, 8.4); `try library.loans.get(copy_id).or_fail(CopyNotOnLoan(...))` (8.3); `Set.empty`, `Set.add`/`has?`/`to_list`, `Map.entries`/`values`/`remove`, `Duration.in_seconds`, `Decimal.ceil`, `n.days` have these signatures (14); `fine`'s examples compute as written (`15.days + 1.seconds` gives `0.50`). |
| Does the task | yes: a catalog of copies, members, `borrow` refusing an unknown member or copy, a copy on loan, or a fourth loan; `return_copy` refusing the wrong borrower and returning the fine, `0.25` for each started day past 14; time is passed in as `now: Time`; promises "a copy is never lent to two members at once" and "a member never has more than three loans"; examples for the fines; three tests. |
| Verdict | close |

Stumbles:
- `anchor_time - 1.seconds`: `Time - Duration` isn't an operator section 3.9 gives (only `Time - Time` and `Time + Duration`); combining different types is an error (3). From other languages' `datetime - timedelta`.

## 16. tictactoe (`tictactoe.mo`, held out)

| | |
| --- | --- |
| Parses | yes |
| Would compile | yes: no mistakes found. The constant `winning_lines` is a `List((Int, Int, Int))` its literal decides (10.4, 3.6), iterated with `for (a, b, c) in winning_lines` (7.5); `board.get(a) == Some(mark)` compares two `Option(Char)`s (9.2); `board.set(index, '.') is Some(previous)` uses `set`'s `Option(List(T))` result (14, 7.4); `continue if ...` and `return ... if ...` guards (5.1); `board.count(fn(cell) cell == 'X' end)` binds `cell` after the loop's `cell` has gone out of scope, and reads only its parameter (6.1, 5.6); `var board = []` is a `List(Char)` from `append` (5.1); bare `return` leaves the `Unit` `main` (5.1, 11.2); the body ends in a one-line `if` with `else` (7.1). |
| Does the task | yes: reads three lines (fewer, or a fourth, is `invalid`), rejects a wrong shape or characters other than `X`, `O`, `.`, impossible counts (`x != o` and `x != o + 1`), both sides winning, or a winner whose count says the other side moved after the win; otherwise prints `X wins`, `O wins`, `draw` on a full board, or `X to move`/`O to move` by the counts. |
| Verdict | pass |

Stumbles: none.

## Summary

| # | Task | Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- | --- | --- |
| 1 | greet | yes | yes | yes | pass |
| 2 | wc | yes | yes | yes | pass |
| 3 | find | yes | yes | yes | pass |
| 4 | expenses | yes | yes | yes | pass |
| 5 | json | yes | yes | yes | pass |
| 6 | todo | yes | no (1: `cause.to_text` on a `FileError`) | yes | close |
| 7 | account | yes | yes | yes | pass |
| 8 | rate-limit | yes | yes | yes | pass |
| 9 | job-queue | yes | yes | yes | pass |
| 10 | kv | yes | yes | yes | pass |
| 11 | notes-tool | yes | yes | yes | pass |
| 12 | inventory | yes | yes | yes | pass |
| 13 | roman (held out) | yes | yes | yes | pass |
| 14 | word-freq (held out) | yes | yes | yes | pass |
| 15 | library (held out) | yes | no (1: `Time - Duration`) | yes | close |
| 16 | tictactoe (held out) | yes | yes | yes | pass |

| Group | pass | close | fail |
| --- | --- | --- | --- |
| Sample tasks (12) | 11 | 1 | 0 |
| Held-out tasks (4) | 3 | 1 | 0 |
| All (16) | 14 | 2 | 0 |

Two compile mistakes in sixteen programs, each a single invented operation on a built-in type:
1. `x.to_text` on a type section 14 gives no `to_text` (`FileError`), where the reference reaches a text form with `"#{x}"` (todo).
2. `Time - Duration`, an operator section 3.9 doesn't give, from other languages' `datetime - timedelta` (library).

Judgment calls that I accepted, for the record: number literals taking `BigInt` from a field's type (3.3, as `Point(x: 0, y: 0)` takes `Decimal`); `[]` with `[first, ..rest]` as complete coverage of a list (7.2, 7.3); `use Unicode16` then `Unicode16.display_width(...)` (10.3, 14); bindings and `var`s inside a promise's `case result` arms (12.2, 7.2).
