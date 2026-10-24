# Grades (grader A)

## qewj2cy
yes
Task 17-stats: adds `median` and `modes` with examples and exposes them; the four existing functions are unchanged.
`median` sorts, takes the middle or the exact mean of the two middle values (no rounding), and returns `None` when empty.
`modes` groups equal values (so 1.50 and 1.5 are one value), keeps every value tied for the top count, and sorts them; an empty list gives `[]`.
Tests for both functions were added to stats-test.mo.

## qut5725
yes
Task 14-wallet: `withdraw` now refuses only when the amount is more than the balance (`checked > wallet.balance`), so withdrawing the whole balance works.
The wallet still can't go below zero for any amount, and the tests are left as they were.

## qw7wjn8
yes
Task 01-rle: `encode` returns `None` for any ASCII digit, writes `count` then `char` for each run (spaces, line breaks and `é` included), and gives `Some("")` for the empty text.
`decode` rejects a missing character, a character with no count, leading zeros or a count of 0, and an overlong count (through `to_int`); `"2a1a"` decodes to `"aaa"`.
Both functions have examples, and `encode` has a round-trip promise.

## qs8qp36
yes
Task 19-cart: `apply_code` matches the three codes exactly, gives `UnknownCode` before checking for a held code, and gives `AlreadyApplied` with the code already held.
`discount` works from the current lines, rounds once with `round(2)` and is capped at the subtotal; a cart with no code has 0. `total` is the subtotal less the discount.
The receipt puts the `discount CODE: -X.XX` line just before the total. The new names are exposed and code tests were added.

## qhtunwa
yes
Task 09-rpn: the program now checks clean (run 002) and the tests pass (run 003), and only rpn.mo changed.
Its behaviour is the reference behaviour: tokens split on white space, the left operand is the deeper value, division rounds toward zero, and errors name the token's position.
`main` prints each value or `error: ...` and returns 1 after any error.

## qf4pume
yes
Task 15-parking: `day_fee` now rounds a started hour up with `(minutes + 59) / 60`, so 31–60 minutes cost one hour and 61 cost two.
This follows the rules in the comments for every stay, with the cap applied per day, and the tests are unchanged.

## q8vmc5s
yes
Task 16-todo: `edit N TEXT...` needs at least one word, joins the words with single spaces, keeps the done state, prints `edited N: TEXT`, and gives `no item N` with status 1 for a bad N.
`clear` drops the done items, keeps the order of the rest, saves the file (empty when none are left), and prints `cleared K done item(s)` with the right plural.
The usage line is updated, `edit 2` and `clear all` fall to usage, and tests were added.

## q5rsj3g
yes
Task 20-timesheet: counted shifts are kept per name, in line order, so a shift sharing a minute (`start < other.finish and other.start < finish`) is reported as `line N: overlaps line M` with the earliest M and isn't counted.
Shifts that only touch don't overlap, names are compared exactly, and the exit status becomes 1.
`--over HOURS` accepts only ASCII-digit whole numbers (huge ones saturate, which is safe) and prints `NAME: TOTAL (EXTRA over)` for totals strictly above HOURS. Any other arguments print the usage line, read nothing, and exit with 2. Tests were added.

## qja2yry
yes
Task 08-playlist: the program checks clean (run 002) and passes the tests (run 003), and only playlist.mo changed. It matches the reference behaviour.
It parses `ARTIST - TITLE (M:SS)` on the first dash and the last bracket, prints the count and total, then `longest: ...`, and reports `skipped: LINE` with status 1.

## qhhjedm
yes
Task 05-ranges: it trims only spaces and tabs (line endings are handled by `read_line`, which the reference relies on too), skips empty lines, and accepts `N` or `LOW-HIGH` made of ASCII digits that fit in an Int, with LOW ≤ HIGH.
Bad lines give `line N: not a range: TEXT` with N counting every line, and status 1.
It merges ranges that overlap or touch in order, prints `N` or `LOW-HIGH`, and `--gaps` prints the ranges between. Any other arguments print the usage line and exit with 2.

## qre3wgg
yes
Task 10-restock: it now checks clean (run 004) and passes every test (run 005), and only restock.mo changed. The code matches the reference.
Rows give `expected 4 fields`, `not a count`, or `bad levels` with status 1; a missing argument or unreadable file gives status 2.

## qhk45pu
yes
Task 15-parking: `day_fee` now counts every started hour with `(minutes + 59) / 60`, the reference fix (plus two extra examples). This follows the rules for every stay, and the tests are unchanged.

## qeud4cz
yes
Task 12-search: the fixed `lower_bound` matches the reference: `high` starts at `items.size`, and the midpoint `low + (high - low) / 2` can't overflow. It still halves, and returns the size when every element is smaller.
`index_of` and `count_between` are right on top of it, and the tests are unchanged.

## qmrmdcw
yes
Task 08-playlist: it checks clean (run 004) and passes the tests (run 005), and only playlist.mo changed. It matches the reference except `try parts.first`, which can't fail because `parts.size >= 2` is checked first.
Output, the `skipped: LINE` errors and the exit status are kept.

## qurz5j8
yes
Task 10-restock: it checks clean (run 002) and passes the tests (run 003), and only restock.mo changed. It matches the reference apart from `count`, which is rewritten as a `case` with the same behaviour (a negative or non-integer field gives `BadNumber` with the trimmed field).

## qezgsdc
yes
Task 16-todo: the `edit` and `clear` code matches the reference (same pattern guard, done state kept, `cleared K done item(s)`, file kept empty), and the usage line is updated.
Bad N gives `no item N` and status 1; `edit 2` and `clear all` give usage. Tests of both commands were added.

## q57absn
yes
Task 20-timesheet: counted shifts are kept in line order, and a later shift of the same exact name that shares a minute is reported as `line N: overlaps line M` (earliest M), isn't counted, and sets status 1. Shifts that only touch don't clash.
`--over HOURS` takes a whole number ≥ 0 (the limit in minutes saturates) and prints `NAME: TOTAL (EXTRA over)` only above it. Other arguments give the usage line, read nothing, and exit with 2. Tests of both were added.

## qxnfv4b
yes
Task 02-col-sum: fields are trimmed of spaces and tabs and parsed strictly (`-`, digits, and an optional `.` with 1–18 digits) into exact scaled BigInts, so `3.`, `.5`, `1e3`, `+1` and the empty field are rejected.
Errors give the field count first, then the leftmost bad field, with line numbers counting the header and skipped lines, and set status 1.
Totals print with two places, a half rounded away from zero (no `-0.00`); no input gives `no header` with status 1.
The one difference is that it skips blank lines before the header, which the general rule that blank lines are skipped allows.

## qnfzjfv
yes
Task 11-calendar: `leap_year?` is now the full Gregorian rule, `year % 4 == 0 and (year % 100 != 0 or year % 400 == 0)`, the same as the reference. It is right for every year, and the other functions build on it. The tests are unchanged.

## q7cjuwz
yes
Task 03-brackets: a stack of open brackets with line and column (counted in characters, a line ending only at `\n`) reports the first problem: `unexpected C`, `C does not match O at L:C`, or the innermost unclosed bracket. Every other character is ignored.
Files are checked in the order given, with `PATH: ok` otherwise. An unreadable file gives `brackets: PATH: no such file` on stderr and the program carries on. Exit is 1 on any problem, and 2 with the usage line when there are no arguments.

## qt5mddg
yes
Task 04-line-counts: lines are counted exactly, or ignoring ASCII case with `--ignore-case`, keeping the first spelling. The output is sorted by count descending with a stable sort (ties keep first-appearance order) and padded to the widest count printed.
`--top N` needs N ≥ 1, and the options may come in either order. Other arguments give the usage line on stderr with exit 2. It also rejects repeated options, which the "anything else is a usage error" rule allows.

## qkewyfs
yes
Task 18-store: `INCR` and `INCRBY` parse the stored value (0 when missing) and the amount with `to_int`, use `checked_add` to give `ERR overflow`, give `ERR not an integer` for bad values, and leave the store unchanged on any error. This is the same as the reference.
`KEYS` lists keys in insertion order (`(empty)` when there are none). The wrong number of words gives `ERR unknown command`, and tests were added.

## qgmq4k9
yes
Task 14-wallet: wallet.mo is the same as the reference: `withdraw` refuses only when `checked > wallet.balance`. The whole balance can be withdrawn and the wallet never goes below zero. The tests are unchanged.

## qzzpf5n
yes
Task 13-titles: `title_case` is the same as the reference. A small word stays lower case only when it is neither first nor last (`index > 0 and index < last`), so the first and last words are always capitalized, for every title. The tests are unchanged.

## q5fje36
yes
Task 02-col-sum: fields and names are trimmed of spaces and tabs only. Numbers are parsed strictly (optional `-`, digits, an optional `.` with 1–18 digits) into exact BigInts scaled by 10^18, and rounded once at the end, a half away from zero, with no `-0.00`.
Errors give the field count first, then the leftmost bad field, with lines counted from 1 including skipped ones, and set status 1. No input gives `no header` with status 1.
Like the reference it treats the first non-blank line as the header, which is consistent with "a line that is empty ... is skipped".

## q6kz5jt
yes
Task 04-line-counts: lines are counted in an insertion-ordered map keyed by the line (ASCII-lowercased with `--ignore-case`), keeping the first spelling. A stable sort by count descending keeps first-appearance order for ties. Counts are padded to the widest printed count, and `--top N` takes the first N.
`--top` needs ASCII digits with a value ≥ 1, and the options come in either order. Anything else gives the usage line and exit 2.

## qaw4kfp
yes
Task 03-brackets: a stack check counts columns in characters and lines only at `\n`, and reports the first problem in the three required forms (the innermost unclosed bracket for "never closed"). Every other character is ignored.
Files are reported in order. An unreadable file goes to stderr as `brackets: PATH: no such file` and checking goes on. Exit is 1 on any problem, and 2 with the usage line when there are no arguments.

## qbqvnvr
yes
Task 18-store: `INCR` and `INCRBY` work like the reference. A missing key counts as "0", both numbers are parsed with `to_int`, `checked_add` gives `ERR overflow`, any unparseable value or amount gives `ERR not an integer`, and the store is unchanged on errors.
`KEYS` uses the store's insertion order (`(empty)` when empty). The wrong word counts give `ERR unknown command`, and tests were added.

## qq4vyv2
yes
Task 13-titles: titles.mo is the same as the reference fix, keeping small words lower case only when they are neither first nor last. The first and last words are always capitalized. The tests are unchanged.

## q85nnx4
yes
Task 07-grades: it checks clean (run 003) and passes every test (run 004), and only grades.mo changed. The code matches the reference apart from a local `count` variable in `average`.
Parsing, rounding to one place, the letters, and `line N: can't read LINE` with status 1 are kept.

## q9e93my
yes
Task 17-stats: `median` sorts, takes the middle value, or for an even count halves the two middle values (splitting off remainders to avoid overflow, which is algebraically exact). It doesn't round and returns `None` when empty.
`modes` counts in a map keyed by Decimal value (its tests show 1.50 and 1.5 are one key), keeps every value tied for the top count, sorts them, and gives `[]` when empty.
Both functions are exposed and have examples, the four originals are unchanged, and tests were added.

## qgrerxb
yes
Task 06-temperature: temperature.mo is identical to the reference. It checks clean (runs 002/003) and passes the tests (run 004). Readings ending in C/F in either case convert to the other scale rounded to one place; anything else gives `not a temperature: TEXT` with status 1.

## qh5m592
yes
Task 11-calendar: `leap_year?` is fixed to the full Gregorian rule, the same as the reference. The tests are unchanged and pass (run 003).
It also rewrote `days_between` as `365·(to−from)` plus a leap-year count using floor division. The formula is correct for every year, including year 0 and negative years, when `to` is the later year, as documented. It differs from the loop only when `from > to`, which is outside the function's contract.

## qk78dmu
yes
Task 07-grades: it checks clean (run 002) and passes every test (run 003), and only grades.mo changed. It matches the reference apart from score validation written as a `case` with the same behaviour (non-integers and scores outside 0–100 make the line unreadable).

## qp2eegm
yes
Task 19-cart: `apply_code` accepts exactly SAVE10/FIVEOFF/BULK, gives `UnknownCode` before checking a held code, and gives `AlreadyApplied` with the held code. Only codes `apply_code` accepts can be stored, so the `Some(_)` arm in `discount` is always BULK.
`discount` is computed from the current lines, rounded once with `round(2)`, and capped at the subtotal (0 with no code). `total` is the subtotal less the discount, and the receipt adds `discount CODE: -X.XX` before the total. CodeError(..) and the new functions are exposed, and tests were added.

## qtpqjpp
yes
Task 01-rle: `encode` writes count-then-char runs over `Char`s (whitespace and Unicode included), returns `None` for any ASCII digit, and gives `Some("")` for the empty text.
`decode` builds the count with `checked_mul`/`checked_add`, so an overflowing count gives `None`. It rejects a leading 0 or a count of 0 (the count is only 0 at the start of a run), a character with no digits before it, and trailing digits. It allows adjacent equal runs.
Both functions have examples, and `encode` has a round-trip promise.

## qz8kje9
yes
Task 12-search: search.mo is identical to the reference fix (`high` starts at `items.size`, with an overflow-safe midpoint). It still halves and is right for every sorted list and number. The tests are unchanged.

## qvvat3e
yes
Task 05-ranges: it trims only spaces and tabs, skips blank lines, and parses `N` or `LOW-HIGH` from ASCII digits that fit in an Int with LOW ≤ HIGH. Bad lines give `line N: not a range: TEXT` with status 1.
It sorts and merges ranges that touch or overlap (with a saturating add at the Int maximum) and prints them, or with `--gaps` only the ranges between them. Any other arguments give the usage line and exit 2 without printing anything else.

## qfjsztf
yes
Task 09-rpn: rpn.mo is identical to the reference except for the order of the `describe` arms. It checks clean (run 003) and passes every test (run 004), and only rpn.mo changed.

## qfhanbe
yes
Task 06-temperature: temperature.mo is identical to the reference. It checks clean (run 003) and passes every test (run 004), and only temperature.mo changed.

## Stumbles

`mo check` diagnostics per session log, by code, and what the session did next. For the "fix compile errors" tasks (rpn, playlist, restock, grades, temperature), the first diagnostics are the planted errors in the starting file.

- qewj2cy (stats): E0021 (`expected end, found )`, stats.mo:77) in its own new code. It fixed the syntax; the next check was clean and the tests passed (15, then 22 after adding tests).
- qut5725 (wallet): none. Test run 10/1 failed → fixed → 11/0.
- qw7wjn8 (rle): none.
- qs8qp36 (cart): none.
- qhtunwa (rpn): E0053, E0050, E0068, E0077 (planted). It fixed all four in one edit; the check was clean and the tests passed 16/0.
- qf4pume (parking): none. Test run 12/1 failed → fixed → 13/0.
- q8vmc5s (todo): none.
- q5rsj3g (timesheet): none. Test runs of its own new tests failed twice (10/2, 12/5); it fixed each, ending 17/0.
- qja2yry (playlist): E0024, E0006 (planted). It fixed both in one edit; the check was clean and the tests passed 19/0.
- qhhjedm (ranges): none.
- qre3wgg (restock): E0023 (planted parse error). Fixing it exposed E0086, E0056, E0070 and E0053. It ran `mo test` once with them still present (0 run), then fixed all four; the check was clean and the tests passed 16/0.
- qhk45pu (parking): none. Test run 12/1 failed → fixed → 15/0.
- qeud4cz (search): none. Test run 11/1 failed → fixed → 12/0.
- qmrmdcw (playlist): E0024, E0006 (planted). It ran `mo test` with them unfixed (0 run), then edited and got E0050 ×2 (an unknown dot-call method at lines 58 and 94, introduced by its own fix). It fixed those; the check was clean and the tests passed 19/0.
- qurz5j8 (restock): E0023 (planted). It fixed it, and the check was then clean (it didn't surface the follow-on errors qre3wgg saw). The tests passed 16/0.
- qezgsdc (todo): none.
- q57absn (timesheet): E0068 (`a choice arm is a catch-all`, timesheet.mo:77) in its own `parse_args` case. It rewrote the arm as `Some(_) | None`; the check was clean and the tests passed 27/0.
- qxnfv4b (col-sum): none.
- qnfzjfv (calendar): none. Test run 16/1 failed → fixed → 17/0.
- q7cjuwz (brackets): none.
- qt5mddg (line-counts): E0007 (`expected =, found :`, line-counts.mo:54) in its own code. It fixed the syntax; the check was clean and the tests passed 23/0.
- qgmq4k9 (wallet): none. Test run 10/1 failed → fixed → 11/0.
- qzzpf5n (titles): none. Test run 10/2 failed → fixed → 12/0.
- qkewyfs (store): none.
- q5fje36 (col-sum): none.
- q6kz5jt (line-counts): none.
- qaw4kfp (brackets): none.
- qbqvnvr (store): none.
- qq4vyv2 (titles): none. Test run 10/2 failed → fixed → 12/0.
- q85nnx4 (grades): E0025 (`Mo writes or instead of ||`, planted). Fixing it exposed E0053, E0050 (name not visible), E0075 and E0051. It fixed all four; the check was clean and the tests passed 18/0.
- q9e93my (stats): none. One test run stopped (20 passed, 1 stopped); it fixed that and ended at 21/0.
- qgrerxb (temperature): E0050, E0070, E0075, E0051 (planted). It fixed all four in one edit; the check was clean and the tests passed 15/0.
- qh5m592 (calendar): none. Test run 16/1 failed → fixed → 28/0.
- qk78dmu (grades): E0025 (planted). It fixed it, and the next check was clean (the follow-on errors q85nnx4 saw didn't appear). The tests passed 18/0.
- qp2eegm (cart): none.
- qtpqjpp (rle): none.
- qz8kje9 (search): none. Test run 11/1 failed → fixed → 12/0.
- qvvat3e (ranges): none.
- qfjsztf (rpn): E0053, E0050, E0068, E0077 (planted). It fixed E0053 and E0050 first; E0068 and E0077 remained, it fixed those, the check was clean and the tests passed 16/0.
- qfhanbe (temperature): E0050, E0070, E0075, E0051 (planted). It fixed E0050 first; E0070, E0075 and E0051 remained, it fixed those, the check was clean and the tests passed 15/0.
