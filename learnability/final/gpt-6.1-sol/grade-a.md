# Final round, GPT-6.1 Sol: grading A

Graded against `docs/reference/README.md` alone, following `learnability/rubric.md`.
"Parses" is given by the executable grammar.

## 1. greet

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes | yes: prints `Hello, NAME!` for the first argument, `Hello, world!` with none | pass |

Stumbles: none.

## 2. wc

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes (the `0` and `1` literals for `BigInt` fields take their type from context, 3.3) | yes: counts newline bytes, white-space-separated words and bytes per file, prints `LINES WORDS BYTES NAME`, a `total` line when more than one file is named, reports an unreadable file on standard error and exits 1 | pass |

Stumbles: none.

## 3. find

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes (the `_` arm at line 55 is over a `List`, not a choice type, so 7.2 allows it) | yes: options `--ignore-case`/`-i`, `--count`/`-c` and `--` before the phrase; prints `FILE:N:LINE`, or `FILE:COUNT` with `--count`; usage errors and unknown options exit 2, an unreadable file is reported and makes the status 2, otherwise 0 when a line matched and 1 when none did | pass |

Stumbles: none.

## 4. expenses

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes (the `case state` lists all four `CsvState` cases; the `_` arms are over `Char` and `List`; `or 0` takes `Decimal` from context) | yes: reads the file named as the one argument, skips the header, parses quoted CSV fields, sums amounts per category with `Decimal`, prints `CATEGORY: TOTAL` with two places largest first, reports each malformed line as `PATH: line N: ...` and exits 1 | pass |

Stumbles: none.

## 5. json

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes (every `case` over `JsonValue` lists its six cases; the name and `_` arms are over `Char`; every `try` passes a `JsonError` up from a function returning `Result(_, JsonError)`; `to_text` customizes the error's text form, 9.2) | yes: a recursive-descent parser over standard input (numbers kept as validated text, exponents and `\u` rejected with a clear error), printed with two-space indentation; a dotted path selects object members and array indexes; bad JSON, a missing path or extra arguments print an error and exit 1 | pass |

Stumbles: none.

## 6. todo

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | no, 1 mistake: line 19, `IoFailure(cause): cause.to_text` with `cause` a `FileError`. Section 14 gives `FileError` no functions, only text forms, so the dot call finds only this module's `to_text`, which takes a `TodoError` (5.4, 14); the text form is reached with `"#{cause}"` (3.4, 9.2) | yes: `add` joins the words and appends, `list` numbers from 1 with a done marker, `done N` and `remove N` check the number; a missing `todo.txt` counts as empty; failures go to standard error with exit 1 via `main : Result(Unit, TodoError)` | close |

Stumbles:
- `cause.to_text` on a `FileError`: no such library function (14), and dot-call lookup finds only the module's `to_text(TodoError)` (5.4); likely generalized from the `to_text` customization hook (9.2) and the numeric types' `to_text`.

## 7. account

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes (each promise has a sentence, a `Bool` body, and `allows`/`forbids` anchors naming every parameter and `result`, and each anchor is accepted or rejected as stated; the `case result` bodies cover every `AccountError`; `is Ok(updated) implies ...` binds as 7.4 allows) | yes: `open`, `balance`, `deposit`, `withdraw`, `transfer` over a record exposed without its fields; withdrawals can't overdraw, negative amounts and overflow are declared failures; promises include that a successful transfer keeps the total (done in `BigInt`); examples and a test agree with the code | pass |

Stumbles: none.

## 8. rate-limit

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes (exact `BigInt` credit in 10^-24 tokens; constants `token_units` and `epoch`, the latter via `or stop`, 8.4, 10.4; both promises have a sentence, a body and anchors for every parameter and `result`, each anchor accepted or rejected as stated; `case` arms inside the promise cover every case; `is` bindings used only after `and`/`implies`, 7.4) | yes, with one wrong test value: a per-client bucket map in a hidden record, time passed in, refill capped at capacity, a refusal gives a rounded-up wait in microseconds, backward time is an error; promises and seven tests. But line 190 expects `Refused(wait_microseconds: 333_333)`: after `ready` is accepted at line 187 the credit is capped at one token (line 123) and drops to 0, so the code gives 333_334 and `mo test` fails there | pass (the flawed test is noted for the third grader) |

Stumbles: none in the language. One logic slip: the test at line 190 forgets the capacity cap that the test at line 193 checks.

## 9. job-queue

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes (five promises, each with a sentence, a body and anchors for every parameter and `result`, built from the private `example_queue`, `example_receipt` and the constant `example_time`; each anchor is accepted or rejected as stated; `case` over `Status` lists its cases; accessor functions share names with `Lease` fields as 5.4 allows; `queue.entries.entries` is the field and then `Map.entries`) | yes: `submit`, `lease` (expires leases first, then the oldest eligible job in submission order, for a duration), `acknowledge`, `fail` and `expire_leases` on an immutable `Queue` with hidden fields; a job gets at most three attempts and is then `Dead`; stale or expired receipts are rejected; promises compare the ordered `(id, payload)` list so that no operation loses a job | pass |

Stumbles: none.

## 10. kv

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes (case names clash with nothing always in scope, 4.2; every `case` over `Command`, `CommandError` and `Reply` lists its cases; text and list `case`s end in `_`; `try` in `parse` stays in `CommandError`; `to_text(Reply)` customizes the text form) | yes: `parse` handles SET, GET, DEL (several keys), INCR and EXPIRE with arity and number checks; `apply` purges expired keys at the given time and returns `(Reply, Store)`; INCR reports non-integers and overflow (`checked_add`); EXPIRE guards the duration and time range; `main` reads lines from standard input with the clock's time and prints each reply | pass |

Stumbles: none.

## 11. notes-tool

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes (`try notes.list(".")` and `try notes.read_text(name)` wrap a `FileError` through the one case `FileFailure(FileError)`, 8.3; the capability arrives as a parameter, 11) | yes: `handle(SearchInput, ReadFiles) : Result(SearchOutput, SearchError)` rejects an empty query and a limit outside 1 to 100, searches the `.md`/`.markdown` files in the folder line by line, returns typed hits (file, 1-based line, text) up to the limit, and sets `truncated?` only when a further match exists; read failures are a typed error | pass |

Stumbles: none.

## 12. inventory

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes (`top_n ... where K: Order` over `sort_by_desc`, `group_items ... where K: Hash` over `group_by`, `deduplicate ... where T: Hash` over `to_set.to_list`; generic `Column(T)` with a function field called as `column.render(item)`, 5.4; `use Unicode16` and `Unicode16.display_width`, 10.3, 14; anonymous-function parameters never rebind a visible name) | yes: generic top-N by a key (stable, largest first), grouping by a key, duplicate removal keeping first occurrences, and a text table whose columns are padded to the widest display width with left or right alignment; `main` applies all of them to a list of products | pass |

Stumbles: none.

## 13. roman (held out)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes (constant `numerals` decides its type, 10.4; the promise has a sentence, two body lines and anchors for `n` and `result`, each accepted or rejected as stated, including `forbids n: 4, result: Some("IIII")` through `from_roman`; `try to_roman(total)` is an `Option` in an `Option` function, 8.3) | yes: `to_roman` is greedy over the 13 symbols for 1 to 3999; `from_roman` consumes symbols greedily, then accepts only when `to_roman` gives back the same text, so `IV` is accepted and `IIII`, `IL`, `IXIX`, `MMMM` and lowercase are rejected; examples for both are correct; the promise states the round trip; `main` prints a numeral, a number or `?` per argument and exits 1 if any failed | pass |

Stumbles: none.

## 14. word-freq (held out)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes (`code.to_char` handled with `case` and `stop`, 8.4; the sort key `(-entry.1, entry.0)` is a tuple, ordered part by part, 9.2; `count` as a local is allowed, 10.5; the `Option` `case` with a guard ends in `Some(_) \| None`) | yes: reads each named file as bytes, takes runs of ASCII letters and `'` as words, lowercased; sorts by count descending, then word by character code; prints `COUNT WORD` for the top 10 or `--top N`; bad options exit 1; an unreadable file is reported on standard error and the status becomes 1 | pass |

Stumbles: none.

## 15. library (held out)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | no, 1 mistake: line 175 (a test), `anchor_time - 1.seconds` subtracts a `Duration` from a `Time`. 3.9 gives only time minus time (a `Duration`), a `Duration` added to a `Time`, and duration arithmetic; combining other types is an error (3, 14 "the operators of section 3.9") | yes: a hidden `Library` value with a catalog of copies, a member set and loans; `borrow` refuses unknown members or copies, a copy already on loan, and a fourth loan; `return_copy` checks the borrower and returns the fine; `fine` charges 0.25 per started day past 14 days, with correct examples; both required promises have sentences, bodies and anchors for every parameter and `result`, built from private constants, each accepted or rejected as stated | close |

Stumbles:
- `anchor_time - 1.seconds`: `Time - Duration` isn't an operation of 3.9 (only `Time - Time`, `Time + Duration`, duration arithmetic); likely from other languages' date arithmetic, where an instant minus a duration is an instant.

## 16. tictactoe (held out)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes (constant `winning_lines` of 3-tuples, 10.4; `board.get(a) == Some(mark)` compares `Option(Char)`s; `board.set(index, '.') is Some(previous)` handles the `Option`, 14; the `for cell` loop has ended before the anonymous functions reuse `cell`, 6.1) | yes: reads exactly three lines (a missing or extra line is `invalid`), checks three characters of `X`, `O` or `.` per line, checks counts (X equals O or one more), rejects two winners, a win with the wrong count, or a win with no last move whose removal leaves no win (a win followed by more moves); otherwise prints `X wins`, `O wins`, `draw`, `X to move` or `O to move` | pass |

Stumbles: none.

## Summary

| # | Task | Set | Parses | Compile mistakes | Does the task | Verdict |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | greet | sample | yes | 0 | yes | pass |
| 2 | wc | sample | yes | 0 | yes | pass |
| 3 | find | sample | yes | 0 | yes | pass |
| 4 | expenses | sample | yes | 0 | yes | pass |
| 5 | json | sample | yes | 0 | yes | pass |
| 6 | todo | sample | yes | 1 | yes | close |
| 7 | account | sample | yes | 0 | yes | pass |
| 8 | rate-limit | sample | yes | 0 | yes (one test asserts a wrong value, line 190) | pass |
| 9 | job-queue | sample | yes | 0 | yes | pass |
| 10 | kv | sample | yes | 0 | yes | pass |
| 11 | notes-tool | sample | yes | 0 | yes | pass |
| 12 | inventory | sample | yes | 0 | yes | pass |
| 13 | roman | held out | yes | 0 | yes | pass |
| 14 | word-freq | held out | yes | 0 | yes | pass |
| 15 | library | held out | yes | 1 | yes | close |
| 16 | tictactoe | held out | yes | 0 | yes | pass |

| Set | pass | close | fail |
| --- | --- | --- | --- |
| Twelve sample tasks | 11 | 1 | 0 |
| Four held-out tasks | 3 | 1 | 0 |
| All sixteen | 14 | 2 | 0 |

Stumbles across the family (two language mistakes, one logic slip):

1. A library function the reference doesn't give: `cause.to_text` on a `FileError` (todo, line 19; 5.4, 14).
2. An operator the reference doesn't give: `Time - Duration` (library, line 175, in a test; 3.9).
3. Not a language mistake: a test value that ignores the program's own capacity cap (rate-limit, line 190), so `mo test` would fail.
