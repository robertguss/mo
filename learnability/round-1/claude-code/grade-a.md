# Round 1, Claude Code: grading A

Graded against `learnability/rubric.md` and the reference `docs/reference/README.md`, and nothing else, by reading.
"Parses" comes from the executable grammar. All twelve answers are graded. rate-limit and job-queue each came
from a second fresh session, because the first ended without writing a file. That session's prompt added one
procedural sentence about working in small steps. They are graded by the same rules.

Every program was checked for these things:
- names, library functions and their signatures, against section 14;
- types: Option and Result values, Int with Decimal, and comparisons;
- `try` and dropped results (8.3, 6.1);
- rebinding and unused names (6.1), including the parameters of anonymous functions;
- assignment only to a `var` (6.2);
- `case` coverage, with no whole-arm catch-all over a choice type (7.2);
- where capabilities and fakes come from (11, 12.4);
- every promise's sentence, body and anchors (12.2). For each anchor, the body was evaluated to confirm that each
  `allows` is accepted and each `forbids` is rejected.

Examples and tests were also traced by hand. All of them hold.

## greet

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes | yes: joins the arguments with spaces and prints `Hello, NAME!`, or `Hello, world!` when there are none or they are blank; exit status 0 | pass |

Stumbles: none.

## wc

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes | yes: lines, words and bytes per file, right-aligned to the width of the total's byte count; a `total` line when more than one file is named; an unreadable file is reported on standard error (`wc: <path>: no such file`), the other files are still counted, and the exit status is 1. With no file named, it counts standard input, which goes beyond the task. | pass |

Stumbles: none.

Checked and correct:
- `files.read_bytes(name).map(tally)` uses Result's `map` (14).
- `None: stop "..."` stands where a value is expected (8.4).
- Fields `lines`, `words` and `bytes` share names with Text's functions, which is allowed (5.4, 10.5).
- The `tally` promise: its one `allows` is accepted and its three `forbids` are rejected (12.2).

## find

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes | yes: prints `FILE:NUMBER:LINE`; `-i`/`--ignore-case` folds ASCII case; `-c`/`--count` prints `FILE:COUNT`; short options combine and `--` ends them; exit status 0 when a line matched, 1 when none did, 2 on a usage error or an unreadable file, and later files are still searched | pass |

Stumbles: none.

Checked and correct:
- Arms whose body is an assignment, such as `"--count": count? = true`, follow `Pattern: statement` (7.2, App. B `arm`).
- `while rest is [word, ..others] and word...` uses its bindings only after `and` and in the loop body (7.4).
- The `case` statements over Text, Char and List end in `_`, which is allowed because none of those is a choice type (7.2).

## expenses

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes | yes: totals by category, largest first (equal totals by name), in two aligned columns; a malformed line is reported on standard error as `line N: ...` with N counting the header as 1, then skipped, and the exit status is 1. Blank lines are skipped. A usage error or an unreadable file gives exit status 2. | pass |

Stumbles: none.

Checked and correct:
- `try amount.to_decimal.or_fail(NotANumber(text: amount))` is in a function returning `Result` (8.1, 8.3).
- Arms that end in `return 2` stand where a value is expected (8.4).
- `group_by` and `sum` (14), then `sort_by` followed by a stable `sort_by_desc` (13.3).
- The literals `800` and `9` take Decimal from the field they fill (3.3).

## json

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes | yes: a recursive-descent parser over `List(Char)`; two-space layout, with `[]` and `{}` for empty containers and numbers kept as written; path steps are object keys or array positions. Invalid JSON, a path with no value, or more than one argument returns `Error`, whose text goes to standard error with exit status 1 (11.2). `\u` is refused and exponents are accepted, both allowed by the task. | pass |

Stumbles: none.

Checked and correct:
- `try parse(...)` in `main : Result(Unit, Problem)` wraps `SyntaxError` through the single case `BadJson(SyntaxError)` (8.3).
- `try` on Options in `lookup` and `child_at`, which both return `Option` (8.3).
- Arms list `Some(_) | None` rather than a catch-all (7.2).
- The module `Json` declares the type `Json` (10.3).
- The case `Number` doesn't clash with the ability, because ability names appear only after `where` (9.2).

## todo

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes | yes: `add TEXT`, `list` (numbered from 1, right-aligned), `done N` and `remove N`, kept in `todo.txt` through `Files`; a bad number, a missing item or empty text goes to standard error with exit status 1; an unknown command prints the usage with exit status 2 | pass |

Stumbles: none.

Checked and correct:
- `try files.write_text(...)` in a function returning `Result(Unit, TodoError)` wraps through the single case `FileTrouble(FileError)` (8.3).
- `items.set` and `remove_at` return `Option(List(T))`, and both are handled with `or_fail` (14).
- Both promises: each `allows` is accepted and each `forbids` rejected (12.2).

## account

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes | yes: `expose Account` hides the fields; `open`, `deposit`, `withdraw` and `transfer` return new values; an overdraft is refused with `InsufficientFunds`, a zero or negative amount with `InvalidAmount`, and a balance too large for an Int with `BalanceTooLarge`. Nine promises include "a transfer neither makes nor loses money", summed in BigInt; examples and tests are included. A core module, with no `main`. | pass |

Stumbles: none.

Checked and correct:
- Fields `owner` and `balance` sit beside exposed functions of the same names (5.4).
- `fn open(owner: Text)` sits in a module with an `owner` function and doesn't call it, which is the reference's own case (6.1).
- All nine promises have a sentence, a body, and anchors that give every parameter and `result` (12.2).
- Anchors build the hidden type inside its own module (10.2).

## rate-limit

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes | yes: one bucket per client under a `Limit` made only by `limit`, with a capacity from 1 to 10^9 and a rate above 0, up to 10^6 a second, at most 6 decimal places (which keeps token counts exact). `request(limiter, client, now)` gives `Allowed(remaining:)` or `Refused(retry_after: Duration)`, rounded up to the millisecond, together with the next limiter. A new client starts full, and a request stamped earlier than the last mints no tokens. Time is always passed in. There are 8 promises and 13 tests, and `prune`, which drops full buckets, goes beyond the task. | pass |

Stumbles: none.

Checked and correct:
- The parameter `limit` in `limiter`, `fill` and `until_token`, and the parameter `limiter` in `request`, `available`, `prune` and `bucket_at`, share names with module functions. None of these functions calls the function it shadows, and `x.limit` is the field (5.4, 6.1).
- Names bound in patterns avoid the visible parameters, as in `BadCapacity(capacity: named_capacity)` (6.1).
- Nested `implies` are parenthesized, because `implies` doesn't chain (App. B).
- `epoch = case Time.parse(...) ... None: stop "..." end` is a valid constant (10.4, 8.4).
- `(now - bucket.updated).in_seconds * limit.per_second` and `((1 - bucket.tokens) * 1000 / limit.per_second).ceil.to_int` stay in Decimal, with no Int mixed in (3.3, 3.9, 14).
- All 28 anchors of the 8 promises were evaluated: each `allows` is accepted and each `forbids` rejected (12.2).

## job-queue

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes | yes: the queue is a value, and `JobQueue`, `JobId` and `Lease` are exposed without insides. The operations are `submit`; `lease(queue, now, length)`, which takes the oldest ready job (a retried job keeps its place); `acknowledge`; `fail`; and `expire(queue, now)`. A job's third failed or expired try makes it `Dead`. Each of the five operations has a "no job is lost" promise: `work(after) == work(queue)`, or the append for `submit`. Done and dead jobs stay in the queue. | pass |

Stumbles: none.

Checked and correct:
- `Lease`'s fields `job_id`, `payload`, `attempt` and `until` sit beside exposed functions of the same names (5.4). `submit(queue, payload: Text)` doesn't call `payload(...)` (6.1).
- `queue.jobs.find(ready?)` resolves to List's `find`, and `after.find(id)` to this module's, by the type of `x` (5.4).
- `var job = try held_job(...)` followed by `job.state = Done`, and `back.state = if ...: Ready else: Dead`, assign fields of a `var` (6.2).
- A one-line `if` is used as an argument, in `Some(if ...: Ready else: Dead)` (7.1, App. B).
- The `expire` promise's multi-line anonymous function binds `(before, after) = pair` and ends in a block `if` (5.6, 7.1).
- The map literal `{id: Job(...)}` uses the value of `id` as its key (3.8).
- All 18 anchors of the 5 promises were evaluated (12.2).

## kv

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes | yes: parses SET, GET, DEL, INCR and EXPIRE in any case, with quoted arguments; `apply(store, command, now)` gives `(Reply, Store)`; INCR overflow replies `(error) ERR increment or decrement would overflow`; expired keys read as missing; `main(console, clock)` serves standard input | pass |

Stumbles: none.

Checked and correct:
- Two `to_text` functions take first parameters of different types declared in this module (5.1).
- The cases `Okay` and `SetValue` avoid `Ok` and `Set`, which are always in scope (4.2).
- `keys.reduce(store, fn(rest, key) ... end)` matches `reduce` (14).
- `longest_ttl` bounds `ttl.seconds` so that it can't overflow (3.9, 13.2).
- `replay`, a helper used by the tests, counts as used (12.3).

## notes-tool

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes | yes: typed `Input(query, limit)`, `Output(hits, truncated?)` and `Hit(file, line, text)`; a typed `SearchError` covers an empty query, a line break in the query, a limit outside 1 to 100, and `Unreadable(FileError)`. It reads only `.md` and `.markdown` files in the folder, through `ReadFiles`, and stops at the first hit past the limit. | pass |

Stumbles: none.

Checked and correct:
- `try notes.list(".")` and `try notes.read_text(name)` wrap through `Unreadable(FileError)` (8.3).
- `use Unicode16` with qualified calls (10.3, 14).
- The anchors give `input`, `notes` (a fake, 12.4) and `result`, and all four check out (12.2).
- The anonymous function in the promise body reads `input`, not the capability (5.6, 11.4).

## inventory

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | yes: no mistakes | yes: generic helpers `top_by` (`where K: Order`), `group_by_key` and `distinct_by` (`where K: Hash`); a generic `table` over `Column(T)` with `cell: fn(T) : Text`, padded by display width; `main` prints the de-duplicated stock list, two top-N lists and category summaries, and a bad N gives exit status 2 | pass |

Stumbles: none.

Checked and correct:
- `column.cell(row)` calls a function held in a field (5.4).
- Every anonymous-function parameter is a name not visible where it is bound (6.1).
- `fn(_letter) 0 end` marks an unused parameter (6.1).
- `use Unicode16` is used (10.3).

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

**pass 12, close 0, fail 0.** No stumbles to log in any of the twelve.
