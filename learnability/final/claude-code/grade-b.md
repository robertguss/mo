# Grade B: Claude Code, final round

Grader B, a fresh session. Judged against `docs/reference/README.md` alone, by
reading, following `learnability/rubric.md`. "Parses" comes from the executable
grammar: yes for all sixteen. Line numbers are those of the answer files.

## 1. greet

| | |
| --- | --- |
| Parses | yes |
| Would compile | yes. Checked: `greeting(arguments: List(Text)) : Text` with a promise (sentence, four `Bool` lines, two `allows` and two `forbids`, each giving `arguments` and `result`; the body accepts both `allows` and rejects both `forbids`), five examples that all hold by reading, and a body using only `join`, `words`, `empty?`, the one-line `if` and interpolation (14, 7.1, 3.4). `main` returns `Unit`, so its tests' calls drop nothing (6.1); `printed` and `printed_errors` are used only in tests (12.4). Nothing unused, nothing rebound. |
| Does the task | yes: `Hello, world!` with no name (or only blank arguments), else `Hello, NAME!` with the arguments joined by one space; exit 0. |
| Verdict | **pass** |

Stumbles: none.

## 2. wc

| | |
| --- | --- |
| Parses | yes |
| Would compile | yes. Checked: `main(args, console, files: ReadFiles) : Int` returns an `Int` on every path (`return 2`, one-line `if`); `files.read_bytes(name).map(tally)` uses `Result.map` with a named function as a value (14, 5.6); both `case`s cover `Ok` and `Error`, with `Error(_)` only inside a pattern (7.2); `name` and `outcome` are bound again only in a separate loop block (6.1); `total.plus(counts)` is a dot call of a module function (5.4); `while data.get(i) is Some(byte)` binds `byte` in the loop (7.4); `Byte.to_int`, `Int.max`, `to_text`, `char_count`, `pad_left`, `join`, `repeat` all exist with these signatures (14); `tally`'s promise has a sentence, three `Bool` lines and anchors giving `data` and `result`, accepted and rejected as required (12.2); all examples hold by reading (byte counts of the `\u{...}` escapes included); `printed`/`printed_errors` only in tests. Nothing unused or rebound. |
| Does the task | yes: one line per readable file (right-aligned counts, then the name), a `total` line when more than one file is named, unreadable files reported on standard error as `wc: PATH: no such file` and status 1; no file named prints usage and exits 2, a sensible choice the task leaves open. |
| Verdict | **pass** |

Stumbles: none.

## 3. find

| | |
| --- | --- |
| Parses | yes |
| Would compile | yes. Checked: `request = case parse_args(...)` with an `Error` arm that ends in `return 2` (8.4); `#{mistake}` uses the module's own `to_text(mistake: UsageError)` (9.2) and `#{failure}` the `FileError` text form (14); the `_` catch-all arms are over a `Text` (`flags`) and a `Char` (`short_flag`), not a choice type (7.2); `case operands` with `[]`, `[_]`, `[phrase, ..paths]` covers every list (7.3); `drop_prefix`, `chars`, `filter_map(short_flag)`, `enumerate`, `filter`, `map`, `contains?` on `Text` and on `List(Flag)` (`Flag` has `Equal`), `ascii_lowercase`, `lines` all exist as used (14); the anonymous functions read only parameters and a fixed name, no `var` and no capability (5.6); `return` inside a `case` arm in a loop is an early exit (5.1). All examples and tests hold by reading (`"-ic"` gives `[IgnoreCase, CountOnly]`, `--colour` gives `None`, `\r\n` lines split). Nothing unused or rebound. |
| Does the task | yes: `FILE:NUMBER:LINE` per match, `-i`/`--ignore-case`, `-c`/`--count` as `FILE:COUNT`, statuses 0/1/2 as asked (an unreadable file gives 2 and the others are still searched); `--` and grouped short options are extras. |
| Verdict | **pass** |

Stumbles: none.

## 4. expenses

| | |
| --- | --- |
| Parses | yes |
| Would compile | yes. Checked: the two `case`s in `main` and the one in `parse_expense` use `_` only over lists (`args.list`, `fields`), not a choice type (7.2), and the `_`/`Error` arms end in `return` (8.4); `try amount_text.to_decimal.or_fail(NotAnAmount(...))` in a function returning `Result(Expense, Problem)` (8.3); `continue if ...` guards (5.1); the literals `0`, `950`, `14` take `Decimal` from their context (3.3); `Map.update(key, default, f)`, `entries`, `sort_by`, `sort_by_desc` (stable, so the name sort then the amount sort orders ties), `List.max or 0`, `pad_left`/`pad_right`, `to_fixed`, Decimal `%` all as in 14 and 3.3; the anonymous functions read only loop bindings, parameters and fixed names (5.6); `#{bad_line.problem}` uses the module's `to_text(problem: Problem)` (9.2); the promise's `is`-bound `expense` is read after `implies` (7.4), and the anchors give `line` and `result`, accepted and rejected as required. All examples and tests hold by reading, including the `"""` text's dropped indentation and `\r\n` lines. Nothing unused or rebound. |
| Does the task | yes: totals per category, largest first (ties by name), each malformed line reported on standard error with its line number and skipped, status 1 then and 0 otherwise; an unreadable file gives 1, a wrong argument count 2, blank lines are passed over. |
| Verdict | **pass** |

Stumbles: none.

## 5. json

| | |
| --- | --- |
| Parses | yes |
| Would compile | yes. Checked: `enum Json` in `module Json` is allowed (10.3), and its cases (`Null`, `Boolean`, `Number`, `String`, `Array`, `Object`) reuse no always-in-scope name; `Number` the ability appears only after `where` (9.2, 4.2); every `case` over an `Option` or `Json` lists all cases, with `None | Some(_)` shared arms and guards (7.2), and `_` whole-arm catch-alls appear only over a `Char`, `Text` or list; `try` is used on `Option`s only in `hex_digit_at` and `step`, which return `Option`s, and on `Result`s whose failure type matches (`ParseError` or `Text`) or after `or_fail` (8.3); tuple targets `(value, after) = try ...` rebind per loop round only (6.1); `is`-bound `c` is read after `and` in `skip_space`/`skip_digits` (7.4); `map_error`, `Text.from_chars`, `Range.to_list`, `filter_map`, `take`, `entries`, `Int.to_char`, `Char.code`/`ascii_digit?`/`ascii_lowercase`, `List.join("")` all as in 14; `'\u{8}'` escapes (3.4); the anonymous functions read only parameters and fixed names and recurse through `layout` (5.6). Examples and tests hold by reading (shortest Decimal spelling `2.5`, surrogate pair to U+1F600, positions `line 1, column 7` and `line 2, column 1`). Nothing unused or rebound. |
| Does the task | yes: standard input is read to its end, parsed, and printed with two spaces per level, or only the value at a dotted path of keys and positions; status 1 for bad JSON or a missing path (reason on standard error), 2 for more than one argument. Exponents and `\u` escapes are supported though not required. |
| Verdict | **pass** |

Stumbles: none.

## 6. todo

| | |
| --- | --- |
| Parses | yes |
| Would compile | yes. Checked: `main : Result(Unit, TodoError)` prints the error's text form via the module's `to_text(failure: TodoError)` (11.2, 9.2); `try files.write_text(...)` in `save` wraps `FileError` into `FileProblem(FileError)`, the one case that holds it as its unnamed value (8.3); `try save(...)` as a statement drops only `Unit` (6.1); `items.set(...) or stop "..."` and `remove_at(...) or stop "..."` fit a value position (8.4); `load`'s `case` lists `Ok`, `Error(NotFound(path: _))` and `Error(problem)` (7.2); `parse_command`'s list patterns end in `[other, .._]` and `[]`, so every list is covered, and `try` inside `MarkDone(number: try parse_number(given))` is an expression (8.3); `to_text`'s `NoSuchItem(..., count: 0)` literal-in-field patterns are followed by the full case (7.3); the `is` tests in `parse_item` bind names in their `if` blocks (7.4); `drop_prefix`, `replace`, `zip`, `enumerate`, `pad_left`, `exists?`, `Files.in_memory({})` as in 14. Both promises have a sentence, a body, and anchors giving `items` and `result`, accepted and rejected as required (12.2). Examples and tests hold by reading (`"[ ] [x] means done"`, the `" 1."`/`"10."` alignment, the fake's file read back). Nothing unused or rebound. |
| Does the task | yes: `add TEXT` (words joined), `list` numbered from 1, `done N`, `remove N`, all kept in `todo.txt` in the start folder; mistakes are reported on standard error with status 1 through the `Result`; a hand-written file and `help` are extras. |
| Verdict | **pass** |

Stumbles: none.

## 7. account

| | |
| --- | --- |
| Parses | yes |
| Would compile | yes. Checked: `struct Account` in `module Account` (10.3); `expose Account` without `(..)` hides the fields while `AccountError(..)` shows its cases (10.1, 10.2); inside the module `account.owner` and `account.balance` are the fields though functions share the names (5.4), and `open(owner: Text)` never calls `owner(...)` (6.1, the reference's own example); `updated.balance = try ... .or_fail(BalanceTooLarge)` and `updated.balance -= amount` assign a `var`'s field (6.2), with `try` on the function's own failure type (8.3); the tuple-in-case patterns `Ok((from_after, to_after))` (7.3) and `assert ... is Ok(ada)` bindings (12.3); `checked_add`, `to_big`, `to_decimal`, `to_fixed` as in 14; `9_223_372_036_854_775_807` fits an `Int` (3.2). Seven promises, each with a sentence, `Bool` lines and anchors giving every parameter and `result`; I checked that each body accepts its `allows` and rejects its `forbids` (e.g. the max-`Int` deposit anchor: `checked_add` is `None`, so the `implies` holds). The `implies` short-circuit (13.1) keeps the `+` and `-` in the bodies from overflowing. All examples and tests hold by reading. Nothing unused or rebound. |
| Does the task | yes: `open`, `deposit`, `withdraw` (refused when it would overdraw, or for an amount not above zero), `transfer` as a withdrawal then a deposit; the type's insides hidden; promises and examples including "a transfer neither makes nor loses money" (summed in `BigInt`). |
| Verdict | **pass** |

Stumbles: none.

## 8. rate-limit

| | |
| --- | --- |
| Parses | yes |
| Would compile | yes. Checked: parameters `limiter`, `capacity` and `per_second` share names with module functions, but no function that names one as a local calls that function; `made.capacity` and the like are field reads where the fields are visible (6.1, 5.4); the constants `noon` (`Time.parse(...) or stop "..."`), `one_a_second` and `ana_empty` have types decided by their values, are computed through `with_bucket` with no cycle, and make no fakes (10.4, 12.4); `is`-bound names (`made`, `left`, `wait`, `ready`, `pause`, `sooner`) are read only after `and` or `implies` in the same line (7.4); `Decimal.floor`, `Decimal.to_int`, `Duration.in_milliseconds`, `Int.milliseconds`, `saturating_add`/`saturating_mul`, `Map.remove`/`keys`/`entries`, `List.to_map`, `Time.checked_add`, `Time - Time`, `Duration - Duration` as in 14 and 3.9; tuple returns, tuple targets `(one, first) = request(...)` and `(_, fourth)` (3.6, 6.1); `assert ... is Ok(start)` bindings (12.3); the anonymous functions read only parameters and `result`, never the `var kept` (5.6); `Limiter`, `Bucket` and `Decision` have `Equal` since `Time`, `Duration` and `Map(Text, Bucket)` do (9.2). Five promises with sentences, bodies and anchors giving every parameter and `result`; I checked that each accepts its `allows` and rejects its `forbids`. All examples and the eleven tests hold by reading (parts counted per millisecond; the 400 ms, 1 ms, 3 s, 11 s and `1s - 600µs` waits). Nothing unused or rebound. |
| Does the task | yes: one bucket per client with a capacity and a per-second rate, a request takes a token or is refused with the wait, time is passed in as a `Time`; promises and tests included, and `forget_full` keeps the limiter from growing. |
| Verdict | **pass** |

Stumbles: none.

## 9. job-queue

| | |
| --- | --- |
| Parses | yes |
| Would compile | no, one mistake. Line 141, `result is Error(BadLength(_)) implies ...`: `BadLength` is declared `BadLength(length: Duration)`, a case with a named field, and 7.3 gives the positional form `Circle(p)` only for a case's one unnamed value, the named form `Rectangle(width: p, ...)` for named fields; a compiler following the reference has no rule by which `BadLength(_)` matches, and would ask for `BadLength(length: _)` (7.3, 4.2). The rest checks out: `expose Queue` hides the queue's fields while `Job(..)`, `State(..)`, `Lease(..)` and the error types show theirs (10.2); `queue.jobs` is the field where the field is visible though `jobs(queue)` is also a function (5.4); `empty()` is a no-parameter call (5.2); `try now.checked_add(length).or_fail(BadLength(...))`, `try oldest_ready(queue).or_fail(NothingReady)` and `try held_job(...)` use the functions' own failure types (8.3); tuple results and `Ok((after, held))` patterns (3.6, 7.3); `is`-bound names (`oldest`, `acknowledged`, `earlier`, `later`, `deadline`, `kept`) read only after `and`/`implies` in the same line (7.4); `queue.jobs.values.filter(finished?)` passes a named function (5.6); the `_` arm in the random test is over an `Int` (7.2); `Random.seeded(2026)` is made only in a test, and every `random.int(low, high)` has `high > low` (12.4, 13.2); `Time.parse(...) or stop` inside `at` (8.4); `zip`, `to_map`, `keys`, `values`, `remove`, `has?`, `take`, `find`, `contains?`, `(-5).seconds`, `1.days` as in 14. Twelve promises with sentences, bodies and anchors giving every parameter and `result`; I checked that each body accepts its `allows` and rejects its `forbids`. All examples and the seven tests hold by reading. Nothing unused or rebound. |
| Does the task | yes: `submit`, `lease` (the oldest ready job, from `now` for a `Duration`, counting the try), `acknowledge`, `fail`, `expire`, a job dead after its third failed try, the queue a value, and a "no job is lost" promise on every operation (plus `take_finished` so the queue doesn't grow). |
| Verdict | **close** (does the task; one compile mistake) |

Stumbles:
- `BadLength(_)` (line 141) for the case `BadLength(length: Duration)`: a named-field case is matched by naming its fields, `BadLength(length: _)` (7.3); likely carried over from the positional `Some(_)`/`Error(_)` habit, since the same file writes `Leased(until: _)` correctly.

## 10. kv

| | |
| --- | --- |
| Parses | yes |
| Would compile | yes. Checked: the two `to_text` functions share a name because their first parameters, `Reply` and `Problem`, are different types declared in this module (5.1), and `#{reply}`/`#{problem}` pick them (9.2); the module's twenty-three cases across five enums are distinct and reuse no always-in-scope name (`Integer`, `Nil`, `Value`, `Failure` are not Mo's) (4.2); `case (name.ascii_uppercase, args)` is over a tuple and `case c` over a `Char`, so their `_` arms are allowed, while the `case`s over `Reading`, `Command`, `Reply`, `Problem`, `Option` and `Result` list every case (7.2, 7.3); `try split_args(line)`, `try integer(seconds)`, `try integer(text)` and `checked_add(1).or_fail(Overflow)` all carry `Problem` (8.3); `at = case deadline(...)` has a `None` arm ending in `return` (8.4); `x or Entry(...)` on an `Option` (8.1); `continue if` guard (5.1); `(reply, updated) = execute(...)` per loop round (6.1); `longest_ttl` is used before its line, which 10.4 allows, and keeps `ttl.seconds` inside a `Duration`; `main(console: Console, clock: Clock)` returns `Unit`, and `Console.fake_input`/`Clock.fixed` appear only in a test (11, 12.4); `ascii_white_space?`, `Text.from_chars`, `ascii_uppercase`, `to_int`, `Map.remove`, `Time.checked_add` as in 14. Three promises with sentences, bodies and anchors giving `text` and `result`, accepted and rejected as required. All examples and the eight tests hold by reading (quote escapes read back, expiry at exactly the deadline, `DEL` counts only live keys). Nothing unused or rebound. |
| Does the task | yes: parses `SET`, `GET`, `DEL`, `INCR`, `EXPIRE` (any case, Redis-style quoting), `apply(store, command, now)` gives `(Reply, Store)`, `INCR` reports overflow and non-integers, and `main` serves standard input line by line with a `Clock`, printing replies as redis-cli does. |
| Verdict | **pass** |

Stumbles: none.

## 11. notes-tool

| | |
| --- | --- |
| Parses | yes |
| Would compile | yes. Checked: the header is module, `expose`, then `use Unicode16`, and `Unicode16.normalize_nfc`/`Unicode16.lowercase` are called through the module as 14 writes them (10.1, 10.3, 14); `try notes.list(".")` and `try notes.read_text(path)` wrap `FileError` into `Unreadable(FileError)`, the one case that holds it, while `try checked_limit(...)` and `try note_paths(...)` carry `SearchError` (8.3); `notes.list(path) is Ok(inner) and not inner.empty?` consumes the `Result` as 7.4's own example does, with `inner` visible in the block; `return ... if` and `continue if`/`break if` guards (5.1, 7.5); the `"\u{FEFF}"` byte order mark is written as an escape (2.1); `drop_prefix(...) or content` (8.1); `words`, `unique`, `concat`, `take`, `enumerate`, `Int.max`, `ReadFiles.within`, `Files.in_memory(...).read_only` as in 14; fakes appear only in promise anchors and tests (12.4); the anonymous functions read only parameters and fixed names (5.6); `#{failure}` uses the module's `to_text(failure: SearchError)` (9.2). Two promises with sentences, bodies and anchors giving every parameter and `result`, accepted and rejected as required. All examples and nine tests hold by reading (sorted listing puts `projects` after `photo.png`, which is skipped since it can't be listed; limit 2 of 3 hits is cut; 20 folders deep passes and 21 is `TooDeep`; `within("..")` gives `Unreadable(NotAllowed(...))`). Nothing unused or rebound. |
| Does the task | yes: `search_notes(notes: ReadFiles, input: SearchInput)` takes a query and a limit from 1 to 100, returns `SearchOutput` with `Hit(file, line, text)` hits and `truncated?`, or a `SearchError`; searches `.md`/`.markdown` files in the folder and its subfolders, skipping hidden names. |
| Verdict | **pass** |

Stumbles: none.

## 12. inventory

| | |
| --- | --- |
| Parses | yes |
| Would compile | yes. Checked: the generic signatures use single-letter type parameters and ask for abilities only where needed, `where K: Order` for `sort_by_desc`, `where K: Hash` for `Map` keys and `Set` members, `where T: Hash` in `dedupe`, whose call of `unique_by` has `K = T` (5.5, 9.2); no generic call makes a bigger type from a parameter (5.5); `key(item)` calls a function-typed parameter, `line_of = fn(cells) ... end` is a local function value called as `line_of(headings)` and passed to `map` (5.6); nested anonymous functions read `i`, `widths`, `table`, loop bindings and fixed names, never a `var` or a capability (5.6, 11.4); `Set.empty` and `{}`/`[]` take their types from use (3.8, 10.4 for the signature-less `var`s inside bodies); `Map.update`, `entries`, `filter_map`, `reduce`, `enumerate`, `concat`, `take(n.max(0))`, `repeat`, `trim_end`, `sum` on `Int` and `Decimal`, `Option.map`, `Unicode16.display_width` through `use Unicode16` as in 14 and 10.3; `Option(Char)` keys have `Hash` since `Char` does, and `{false: ..., true: ...}` is a `Map(Bool, List(Int))` compared in any order (9.2, 3.8). All examples hold (stable `sort_by_desc` keeps `kiwi` before `pear`); the six tests hold by reading, each rendered table checked cell by cell (widths 8/3, 4/5, 6/12/9/6/5/7, 11/8/5/7/13; values 1398.60, 1046.50, 942.50; category sums 566.88, 1844.50, 2341.10, 662.10, 885.00, total 6299.58). `Console.fake()` only in a test (12.4). Nothing unused or rebound. |
| Does the task | yes: `top_by` (the top N by a key), `group_by_key`, `unique_by`/`dedupe` (and `repeated_keys`), a text table with left/right-aligned columns measured in screen columns, all used on a product list by `main` to print a stock report. |
| Verdict | **pass** |

Stumbles: none.

# The four held-out tasks

## 13. roman (held out)

| | |
| --- | --- |
| Parses | yes |
| Would compile | yes. Checked: `symbols` is a constant `List((Int, Text))` decided by its value (10.4) and walked with `for (value, symbol) in symbols` (7.5, 3.6); `value = try symbol_value(symbol)` is an `Option` `try` in a function returning an `Option` (8.3); `result != None` and `to_roman(total) == Some(text)` compare `Option`s that have `Equal` (9.2); the `_` arm in `symbol_value` is over a `Char` (7.2); `Option.map`, `chars`, `reverse`, `byte_count`, `to_int`, `to_text`, `x or "?"` as in 14 and 8.1; `assert to_roman(number) is Some(numeral)` binds per loop round (12.3, 6.1); `main : Int` returns `status` on every path (11.2). Three promises with sentences, `Bool` bodies and anchors giving `n`/`text` and `result`; each accepts its `allows` and rejects its `forbids` (e.g. `from_roman("IIII")` is `None`, so `Some(4)` is rejected). All examples hold by reading (right-to-left subtraction, then the text must equal `to_roman(total)`), and the four tests hold. Nothing unused or rebound. |
| Does the task | yes: `to_roman(n: Int) : Option(Text)` for 1 to 3999, `from_roman(text: Text) : Option(Int)` accepting only the standard numeral (`IV`, not `IIII`), examples for both, the round-trip promise on `to_roman`, and a `main` that converts each argument either way, prints `?` for one it can't, and exits with 1 if any couldn't be converted. |
| Verdict | **pass** |

Stumbles: none.

## 14. word-freq (held out)

| | |
| --- | --- |
| Parses | yes |
| Would compile | yes. Checked: `request = case parse_args(...)` with an `Error` arm ending in `return 2` (8.4); `parse_args` recurses with `try parse_top(value)` inside its arguments, both failing with `Text` (8.3), and its list patterns `[]`, `["--", ..rest]`, `["--top"]`, `["--top", value, ..rest]`, `[arg, ..rest]` cover every list, with the inner `None if ...` guard and plain `None` covering the `Option` (7.2, 7.3); the `var counts` of `main` is assigned inside a nested loop of the same function (6.2); `word` is bound in two separate loop blocks (6.1); `data.decode_utf8 or latin1_text(data)` and `code.to_char or ' '` (8.1); `stop` in a `case` arm (8.4); `sort_by(fn(entry) (-entry.1, entry.0) end)` orders by a `(Int, Text)` key, and tuples have `Order` part by part (9.2, 14); `take(top)` gets a `top` of 0 or more from `parse_top`; `read_bytes`, `Bytes.get`, `Byte.to_int`, `Text.from_chars`, `Char.ascii_letter?`/`ascii_lowercase`, `words`, `Map.update`, `entries`, `drop_prefix`, `'\''` as in 14 and 3.4; capabilities reach `read_source`/`read_all` only as parameters (11.1); fakes only in tests (12.4). All examples and the five tests hold by reading (`"3 the", "2 hat", "1 and", "1 cat", "1 cat's"`; `--top lots` gives status 2 with the usage). Nothing unused or rebound. |
| Does the task | yes: counts words (runs of ASCII letters and apostrophes, lowercased) across the named files, prints the N most frequent as `count word`, most frequent first and ties in alphabetical order, N being 10 or `--top N`; an unreadable file is reported on standard error and the status is 1. Standard input with no files, `--top=N` and `--` are extras. |
| Verdict | **pass** |

Stumbles: none.

## 15. library (held out)

| | |
| --- | --- |
| Parses | yes |
| Would compile | yes. Checked: `struct Library` in `module Library`, exposed without its insides (10.3, 10.2); `receipt.fine` is the field where the field is visible, and `fine(now - loan.since)` calls the function; no function names `fine` as a local (5.4, 6.1); the constants `loan_period = 14.days`, `daily_fine = 0.25`, `empty`, `opening` (a `case` on `Time.parse` with a `stop` arm), `dune`, `emma` and `sample` (map literal with the expression keys `dune.isbn`, `emma.isbn`, and `["ana", "ben"].to_set`) have types decided by their values and no cycle (10.4, 3.8, 8.4); `try library.loans.get(copy).or_fail(NotLent(...))` carries `ReturnError`, and `try library.copies.get(copy)` is an `Option` `try` in `book_of` (8.3); `updated.loans = ...` assigns a `var`'s field (6.2); `Duration.in_seconds` is an exact `Decimal`, so `(over.in_seconds / 1.days.in_seconds).ceil * daily_fine` is a `Decimal`, and `return 0 if ...` gives a `Decimal` `0` (3.3, 14); `Set.add`/`has?`, `Map.set`/`remove`/`has?`/`keys`/`values`, `count`, `filter`, `sort_by` on `Time` keys, `Time - Time`, `Time + Duration` as in 14 and 3.9; the anonymous functions read parameters and `is`-bound names (5.6, 7.4); `Receipt`, `Library` and the errors have `Equal` (9.2). Four promises with sentences, bodies and anchors giving every parameter and `result`; each accepts its `allows` and rejects its `forbids` (e.g. `lent_to("ben", ["dune-1"])` already holds the copy, so the `Ok` is rejected). All fine examples hold (1 s over gives `0.25`, 6 h over 15 days gives `0.50`, 20 days gives `1.50`) and the five tests hold by reading. Nothing unused or rebound. |
| Does the task | yes: a catalog of books and copies, members who borrow at most three copies and return them, a fine of 0.25 for each day (or part) past 14, time passed in to `borrow`, `return_copy` and `overdue`; promises "a copy is never lent to two members at once" and "a member never has more than three loans"; examples for the fines. |
| Verdict | **pass** |

Stumbles: none.

## 16. tictactoe (held out)

| | |
| --- | --- |
| Parses | yes |
| Would compile | yes. Checked: `winning_lines` is a constant `List(List(Int))` decided by its value (10.4); `main(console: Console)` reads standard input with `read_line` and returns `Unit` (11.2, 14); `#{outcome}` uses the module's `to_text(outcome: Outcome)` (9.2); `case lines.last` lists `Some(line) if ...` and `Some(_) | None`, and `case read_board(lines)` lists `Some`/`None` (7.2); `rows.all?(valid_row?)` passes a named function, and the nested anonymous functions in `has_line?` read only the parameters `cells` and `player` (5.6); `flat_map`, `count`, `take`, `drop`, `last`, `any?`, `all?`, `chars`, `trim` as in 14; the `return ... if` guards (5.1); `Wins(player)` is a positional pattern on a case with one unnamed value (7.3); `Outcome` has `Equal` and `Char` has `Text` (9.2). The promise has a sentence, three `Bool` lines and anchors giving `lines` and `result`, accepted and rejected as required. All examples and the four tests hold by reading (`XOX/OXO/OXX` is `Wins('X')` with 5 X and 4 O; `OOO/XX./XX.` is invalid since X moved after O's win). Nothing unused or rebound. |
| Does the task | yes: three lines of `X`, `O`, `.` from standard input; prints exactly `X wins`, `O wins`, `draw`, `X to move`, `O to move` or `invalid`; invalid for a wrong shape or character, impossible counts (X must equal O or O + 1), both sides winning, or a win followed by a move; blank lines after the board are tolerated. |
| Verdict | **pass** |

Stumbles: none.

# Summary

| # | Task | Verdict | Compile mistakes |
| --- | --- | --- | --- |
| 1 | greet | pass | 0 |
| 2 | wc | pass | 0 |
| 3 | find | pass | 0 |
| 4 | expenses | pass | 0 |
| 5 | json | pass | 0 |
| 6 | todo | pass | 0 |
| 7 | account | pass | 0 |
| 8 | rate-limit | pass | 0 |
| 9 | job-queue | close | 1 (`BadLength(_)` for a named-field case, 7.3) |
| 10 | kv | pass | 0 |
| 11 | notes-tool | pass | 0 |
| 12 | inventory | pass | 0 |
| 13 | roman (held out) | pass | 0 |
| 14 | word-freq (held out) | pass | 0 |
| 15 | library (held out) | pass | 0 |
| 16 | tictactoe (held out) | pass | 0 |

| | pass | close | fail |
| --- | --- | --- | --- |
| Sample tasks (12) | 11 | 1 | 0 |
| Held-out tasks (4) | 4 | 0 | 0 |
| All (16) | 15 | 1 | 0 |

Every program parses, does its task by reading, and has promises, examples and
tests that hold. Across the sixteen programs I found one compile mistake, so
there is one kind of stumble, not five:

1. A positional pattern on a case declared with a named field: `BadLength(_)`
   for `BadLength(length: Duration)` (job-queue, line 141), where 7.3 gives
   the positional form only for a case's one unnamed value. The same file
   writes `Leased(until: _)` correctly, so this is a slip, not a
   misunderstanding.

Points I weighed and resolved in the programs' favour, listed so a third grader
can check them; none is counted as a mistake:

- `Result(T, Text)` as a failure type (json's `run`, word-freq's
  `parse_args`): 8.2 describes declaring failures as a choice type, but states
  no rule against a `Text` failure.
- Testing a `Result` with `is Ok(...)` and doing nothing for the `Error`
  (notes-tool, `notes.list(path) is Ok(inner) and ...`): 7.4's own example
  does the same, and the result is used, not dropped.
- Parameters named after module functions (`limiter`, `capacity`,
  `per_second` in rate-limit; `open(owner: Text)` in account): 6.1 allows this
  as long as the function doesn't call the shadowed function, and none does;
  `x.capacity` and the like are field reads where the field is visible (5.4).
- Case names `Number`, `Integer`, `String`, `Array`, `Object`, `Nil`, `Value`
  (json, kv): none is a type or case that is always in scope (4.2, 10.5), and
  ability names appear only after `where` (9.2).
- Every whole-arm `_` catch-all is over a `Text`, `Char`, `Int`, list or tuple,
  never a choice type (7.2).
- Constants computed by calling functions (`noon`, `ana_empty`, `opening`,
  `sample`): 10.4 allows constants to use functions and later constants, and
  none makes a fake (12.4).
