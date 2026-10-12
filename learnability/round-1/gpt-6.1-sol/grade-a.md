# Round 1, gpt-6.1-sol: grade A

Graded by reading, against `docs/reference/README.md` alone, following
`learnability/rubric.md`. "Parses" comes from the executable grammar.

For every program I checked the rubric's list: names and library functions
against section 14; types (an `Option` or `Result` used as a plain value, mixed
number types, comparisons across types); failures handled or passed upward
(8.3) and no non-`Unit` result dropped (6.1); naming, rebinding and unused names
(6.1); assignment only to a `var` or its fields (6.2); `case` coverage and
catch-alls (7.2); capabilities and fakes (11, 12.4); and promise parts, with
every `allows` and `forbids` anchor evaluated by hand against its body (12.2).
I also checked reserved words (2.3), invisible characters (2.1), guards only on
exits (5.1), `x.f` without `()` (5.4), records built with every field (4.1),
case names (4.2), header order (10.1) and the dot-call/field rules (5.4).

## 1. greet

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes | yes: prints `Hello, NAME!` for the first argument, `Hello, world!` with none | pass |

Stumbles: none.

## 2. wc

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes | yes: for each file prints `lines words bytes name` from `read_bytes` (lines = LF bytes; words = runs between bytes 9-13 and 32); a `total` line when more than one file is named; an unreadable file's `FileError` goes to stderr and the status becomes 1. No arguments gives usage and status 1 (reading stdin isn't asked for) | pass |

Checked and accepted: record fields `lines` and `bytes` named like `Text`
functions (5.4: the field wins where it is visible).

Stumbles: none.

## 3. find

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes | yes: `-i`/`--ignore-case` (ASCII-lowercases both sides), `-c`/`--count`, `--`; prints `path:N:line`, or `path:count` per file; status 2 on a usage error or an unreadable file (takes precedence), else 0 if any line matched, else 1. Options are read only before PHRASE | pass |

Checked and accepted: `try arguments.get(position)` in a function returning
`Option` (8.3); a local named `count` (10.5).

Stumbles: none.

## 4. expenses

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes | yes: skips the header; splits CSV, including quoted fields; an amount is an optional `-`, digits and up to 2 decimals, held as `BigInt` cents; a malformed line gives `path:N: malformed expense` on stderr, is skipped, and sets status 1; prints `category,total` largest first (`sort_by_desc`) | pass |

Checked and accepted: `amount * 10`, `amount < 0`, `magnitude / 100` on
`BigInt` (a literal takes the type its context needs, 3.3); `case` over a `List`
with a `_` arm (7.2 bans catch-alls only over choice types); `ch` used by two
anonymous functions and then by a `for` (separate blocks, 6.1).

Stumbles: none.

## 5. json

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes | yes: parses null, booleans, numbers (as `Decimal`), strings with the simple escapes, arrays and objects, and reports positions in errors; rejects exponents and `\u` escapes (the task allows that); prints with two-space indentation; an optional argument such as `items.0.name` selects by object key and array index; errors go to stderr with status 1. Numbers are printed in `Decimal`'s shortest form (`1.50` prints as `1.5`), which keeps the value | pass |

Checked and accepted: char literals with escapes, such as `'\u{8}'`, as
patterns (3.4, 7.3); `case` over a `Char` with `_` and `other` arms (not a
choice type, 7.2); `entries.entries` on a local named like a `Map` function
(10.5); `stop` after `while true` (8.4); `ch` bound by three separate `is`
tests (7.4: each is visible only in its own block).

Stumbles: none.

## 6. todo

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes | yes: `add TEXT`, `list` (numbered from 1), `done N` and `remove N`, kept in `todo.txt` as `[ ] TEXT` and `[x] TEXT` lines; a missing file is an empty list; failures leave through `main : Result(Unit, TodoError)` with a custom `to_text`, status 1 | pass |

Checked and accepted: `try files.write_text(...)` in a
`Result(Unit, TodoError)` function wraps through the one case holding a
`FileError` unnamed, `Storage(FileError)` (8.3); a local `index` in `load` next
to the module function `index`, which `load` doesn't call (6.1); `body` bound
in `if ... is Some(body)` and again in the `else if` (7.4); `position` and
`updated` bound in two separate arms.

Stumbles: none.

## 7. account

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes | yes: `expose Account` hides the fields; `open`, `deposit`, `withdraw` and `transfer` return `Result(_, AccountError)`, and a withdrawal above the balance gives `InsufficientFunds`; all four have a promise (a sentence, a body, and `allows` and `forbids` giving every parameter and `result`) and examples; `transfer`'s promise includes `debited.cents + credited.cents == source.cents + destination.cents` | pass |

Checked and accepted: each body accepts its `allows` and rejects its
`forbids`; `BigInt` compared with `0` (3.3); `debited` and `credited` bound in a
promise arm and again in the body (an arm's names end with the arm, 6.1); a
module and a type both named `Account` (10.3).

Stumbles: none.

## 8. rate-limit

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes | yes: one `Bucket` (`Float` tokens and last update) per client in a hidden `Limiter`; new clients start full; refill is elapsed seconds × rate, capped at capacity; `request(limiter, client, now)` gives `Accepted` or `Refused(wait_seconds)`, or `TimeWentBackwards`; time is passed in; promises on `create` and `request`; two tests, whose expected waits (0.5, 0.25, 1.0) are exact in binary | pass |

Checked and accepted: the constant `epoch = Time.parse(...) or stop "..."`
(8.4, 10.4); records with `Float` fields compared only with `==` (9.2: `Float`
has `Equal`, not `Hash`); `case decision ... end` nested in a promise arm;
anchors evaluated.

Stumbles: none.

## 9. job-queue

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes | yes: a hidden `Queue` value keeps a ledger of jobs; `submit`; `lease(queue, now, duration)` takes the first `Ready` job, counts the attempt and gives a token and a deadline; `acknowledge` and `fail` expire leases first and check the token; `expire` puts an expired lease back to `Ready`, or `Dead` after the third attempt (`fail` does the same); promises on all five operations say every job (id and payload) is kept and the count holds; two tests, both traced correct | pass |

Checked and accepted: the module function `jobs` next to the field `jobs`
(5.4); `+= 1` on a `BigInt` field (3.3); `case` arms ending in `return` inside a
`for` (8.4); anchors evaluated.

Stumbles: none.

## 10. kv

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes | yes: a tokenizer with quotes and backslash escapes; `parse` gives `SET`, `GET`, `DEL`, `INCR` or `EXPIRE` (names case-insensitive) or `BadCommand`; `apply(store, command, now) : (Reply, Store)` with lazy expiry; `INCR` uses `checked_add` and replies `IncrementOverflow`; `EXPIRE` guards against `Duration` overflow; `main` reads commands from stdin, gets the time from a `Clock`, and prints each reply | pass |

Checked and accepted: two `to_text` functions whose first parameters are
different types of the module (5.1); `seconds.seconds` resolves to `Int`'s
function (5.4, 10.5); `var quote = None` typed by its later uses (5.1).

Stumbles: none.

## 11. notes-tool

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes | yes: `search(input: SearchInput, notes: ReadFiles) : Result(SearchOutput, SearchError)`; the query must be non-empty and the limit from 1 to 100; reads the folder's `.md` files in `list` order, line by line, ignoring ASCII case; returns `Hit(file, line, text)`s, with `truncated?` set when another match passes the limit; typed errors for bad input and file failures; the fake folder is made only in the test | pass |

Checked and accepted: `FileError`s handled with `case` and mapped to
`FolderRead` or `NoteRead(file, problem)`.

Stumbles: none.

## 12. inventory

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes | yes: `top_n` (`where K: Order`; `sort_by_desc`, then `take`), `group_items` (`where K: Hash`), `deduplicate` (`where T: Hash`), and `table`, which aligns columns by `Unicode16.display_width` under a dashed rule; `main` uses them all on a product list that holds a duplicate | pass |

Checked and accepted: `fn(_) 0 end` (`_` means "ignored", 2.3);
`use Unicode16` then `Unicode16.display_width` (10.3, 14); `count.max(0)` keeps
`take` in range (13.2); the test's expected table traced by hand.

Stumbles: none.

## Summary

| Program | Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- | --- |
| greet | yes | yes | yes | pass |
| wc | yes | yes | yes | pass |
| find | yes | yes | yes | pass |
| expenses | yes | yes | yes | pass |
| json | yes | yes | yes | pass |
| todo | yes | yes | yes | pass |
| account | yes | yes | yes | pass |
| rate-limit | yes | yes | yes | pass |
| job-queue | yes | yes | yes | pass |
| kv | yes | yes | yes | pass |
| notes-tool | yes | yes | yes | pass |
| inventory | yes | yes | yes | pass |

**Pass 12, close 0, fail 0.**

Stumble log: empty. No form a compiler following the reference would reject.

The closest calls, all accepted under the sections cited, for whoever settles
a disagreement:

1. A bare literal used where the context is `BigInt` or `Float`
   (`amount * 10`, `result.cents == 0`, `next_id += 1`, `capped >= 1`):
   expenses, account, job-queue, rate-limit. Allowed by 3.3, "a number literal
   takes the type its context needs".
2. A local or field named like a library function (`first`, `count`,
   `seconds`, `lines`, `chars`, `entries`, `error`, `bytes`): wc, find,
   expenses, json, todo, job-queue, kv, inventory. Allowed by 10.5 and 5.4.
3. One name bound in separate blocks (two `case` arms, a promise arm and the
   body, an `if ... is` and its `else if`, an anonymous function and a later
   loop): expenses, json, todo, account, rate-limit, kv, inventory. Allowed by
   6.1 and 7.4.
4. A whole-arm catch-all (`_` or a name) over a `List` or `Char`: expenses,
   json, todo, kv. Allowed by 7.2, which bans this only over choice types.
5. A `var` that starts as `[]`, `{}` or `None`, with its type fixed by later
   uses: expenses, json, todo, kv, notes-tool. Allowed by 5.1, "types are
   inferred from all their uses".
