# Final round, Claude Code family: grading A

Grader A. Each program is judged against `docs/reference/README.md` (v0.1) alone and
graded by `learnability/rubric.md`. "L12" means line 12 of the program. Section
numbers are the reference's. "Parses" comes from the executable grammar.

## 1. greet (sample)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes found | yes: greets the joined argument words, or "world" when there are none or they are blank; exit 0 | pass |

Stumbles: none.

## 2. wc (sample)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes found | yes: line, word and byte counts per file in aligned columns, a total when more than one file is named, an unreadable file reported on standard error with exit 1 (no file named: usage and exit 2, a sensible choice) | pass |

Stumbles: none.

## 3. find (sample)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes found | yes: prints `FILE:N:LINE`, `-i`/`--ignore-case` folds ASCII case, `-c`/`--count` prints `FILE:COUNT`; exit 0 on a match, 1 on none, 2 on a usage error or an unreadable file | pass |

Stumbles: none.

## 4. expenses (sample)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes found | yes: skips the header, totals by category with `Map.update`, prints largest first (ties by name) in aligned columns; a malformed line is reported with its number (header is line 1) and skipped, exit 1; an unreadable file exits 1, a wrong argument count 2 | pass |

Stumbles: none.

## 5. json (sample)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes found | yes: parses standard input (with positions in errors), prints it with two-space indentation, or the value at a dotted path such as `items.0.name`; exit 1 on bad JSON or a missing path, 2 on extra arguments. Numbers that fit a `Decimal` are reprinted in shortest form (`2.50` as `2.5`), a defensible choice | pass |

Stumbles: none.

## 6. todo (sample)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes found | yes: `add`, `list` (numbered from 1), `done N`, `remove N` on `todo.txt`, a missing file read as an empty list; errors reach `main`'s `Result` and exit 1 | pass |

Stumbles: none.

## 7. account (sample)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes found | yes: `Account` exposed without its insides; `open`, `deposit`, `withdraw` (refuses to overdraw), `transfer` (withdraw then deposit, all or nothing); promises with valid anchors, including "a transfer neither makes nor loses money" (summed as `BigInt`), and examples | pass |

Stumbles: none.

## 8. rate-limit (sample)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes found | yes: one bucket per client in a hidden `Limiter`, capacity and per-second rate checked at creation, tokens kept exactly in parts; `request(limiter, client, now)` gives the next limiter and `Allowed(remaining)` or `Refused(retry_after)`; time passed in; promises with valid anchors and eleven tests | pass |

Stumbles: none.

## 9. job-queue (sample)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes found | yes: `Queue` is a hidden value; `submit`, `lease` (oldest ready job, from `now` for a length), `acknowledge`, `fail`, `expire`, plus `take_finished`; a job's third failed or expired try leaves it `Dead`; every operation has a "no job is lost" promise (`keeps_every_job?`) with valid anchors, and the tests include a seeded random run | pass |

Stumbles: none.

## 10. kv (sample)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes found | yes: parses SET, GET, DEL, INCR, EXPIRE lines (any case, quoted arguments) into a `Command`; `apply(store, command, now)` gives `(Reply, Store)`; expiry checked lazily at `now`; INCR gives `Failure(Overflow)` via `checked_add`; `main` serves standard input with `Clock` time and redis-cli-style replies | pass |

Stumbles: none.

## 11. notes-tool (sample)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes found | yes: `search_notes(notes: ReadFiles, input: SearchInput)` gives `Result(SearchOutput, SearchError)`; the limit is checked from 1 to 100; Markdown notes are walked recursively (hidden names skipped, depth bounded); typed `Hit(file, line, text)` and `truncated?` (one hit past the limit is collected to know); `FileError` wrapped as `Unreadable` | pass |

Stumbles: none.

## 12. inventory (sample)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes found | yes: generic `top_by` (`where K: Order`), `group_by_key`, `unique_by`/`dedupe` (`where K: Hash`), `repeated_keys`; a `Table` renderer with left/right alignment, rules and a footer, measured with `Unicode16.display_width`; `main` uses them on a product list | pass |

Stumbles: none.

## 13. roman (held out)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes found | yes: `to_roman(n: Int) : Option(Text)` for 1 to 3999; `from_roman(text: Text) : Option(Int)` accepts only standard numerals (it re-checks with `to_roman`, so `IIII` gives `None`); examples for both; round-trip promise on `to_roman` (and its converse on `from_roman`); `main` converts each argument, prints `?` on failure and exits 1 if any failed | pass |

Stumbles: none.

## 14. word-freq (held out)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes found | yes: words are runs of ASCII letters and apostrophes, lowercased; counts across all named files; prints `count word`, most frequent first with alphabetical ties (sort key `(-count, word)`); N is 10 or `--top N`; an unreadable file is reported and the others counted, exit 1 (extras: `--top=N`, standard input when no file is named, usage errors exit 2) | pass |

Stumbles: none.

## 15. library (held out)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes found | yes: a hidden `Library` with books, copies, members and loans (kept by copy); `borrow` refuses unknown members or copies, copies on loan, and a fourth loan; `return_copy` gives a `Receipt` with the fine, 0.25 per day (part days rounded up) past 14 days; time passed in; both required promises on `borrow` with valid anchors; examples for `fine` | pass |

Stumbles: none.

## 16. tictactoe (held out)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes found | yes: reads standard input; rejects a wrong shape (trailing blank lines allowed) or wrong characters; checks the X and O counts, both players winning, and moves after a win (`x_won? and crosses == noughts`, `o_won? and crosses > noughts`); prints `X wins`, `O wins`, `draw`, `X to move`, `O to move` or `invalid` through `to_text(Outcome)` | pass |

Stumbles: none.

## Summary

| # | Program | Set | Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | greet | sample | yes | yes | yes | pass |
| 2 | wc | sample | yes | yes | yes | pass |
| 3 | find | sample | yes | yes | yes | pass |
| 4 | expenses | sample | yes | yes | yes | pass |
| 5 | json | sample | yes | yes | yes | pass |
| 6 | todo | sample | yes | yes | yes | pass |
| 7 | account | sample | yes | yes | yes | pass |
| 8 | rate-limit | sample | yes | yes | yes | pass |
| 9 | job-queue | sample | yes | yes | yes | pass |
| 10 | kv | sample | yes | yes | yes | pass |
| 11 | notes-tool | sample | yes | yes | yes | pass |
| 12 | inventory | sample | yes | yes | yes | pass |
| 13 | roman | held out | yes | yes | yes | pass |
| 14 | word-freq | held out | yes | yes | yes | pass |
| 15 | library | held out | yes | yes | yes | pass |
| 16 | tictactoe | held out | yes | yes | yes | pass |

| Set | Pass | Close | Fail |
| --- | --- | --- | --- |
| Sample tasks (12) | 12 | 0 | 0 |
| Held-out tasks (4) | 4 | 0 | 0 |
| All (16) | 16 | 0 | 0 |

Stumbles: none in any of the sixteen programs, so the diagnostic log gets no entries
from this family.

Besides reading every line, these checks were run across all sixteen files: no
`x.f()` dot calls with empty parentheses, no habits from other languages (`&&`, `||`,
`elif`, `=>`, `->`, `let`, `null`, indexing, `+` on text), every `try` in a function
whose `Result` or `Option` matches or wraps through exactly one case (8.3), no `try`
in a test or anonymous function, no non-`Unit` result dropped (6.1), no invisible or
control characters (2.1), and every promise anchor accepted or rejected as labelled
(12.2).

Rule boundaries the programs came close to and stayed within (not mistakes):

- A parameter sharing a module function's name: `open(owner: Text)` (account) and
  `limiter(capacity, per_second)`, `capacity(limiter)` and others (rate-limit). Legal
  by 6.1, since no such function calls the function it shadows.
- A record field sharing a function's name: `receipt.fine`, `queue.jobs`,
  `account.owner`, `limiter.capacity`. The field wins inside the module (5.4).
- Two `to_text` functions in one module (kv: `Reply` and `Problem`). Legal by 5.1.
- The same name bound by `is` on several promise lines (`after`, `held`,
  `from_after`). Each binding is visible only after its own `implies` (7.4).
- A whole-arm `_` used only over `Text`, `Char`, `Int`, lists and tuples, never over
  a choice type (7.2).

Open points in the task texts, settled sensibly (not counted against any program):
expenses doesn't check the date field's format; library charges part days as whole
days; json reprints numbers in shortest form; wc exits 2 when no file is named;
word-freq reads standard input when no file is named.
