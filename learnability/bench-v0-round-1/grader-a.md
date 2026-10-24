# Grades

## quq3hcv
yes
- `median` sorts, returns the middle value for an odd count and the exact mean of the two middle values for an even count (`halfway` is exact), `None` when empty.
- `modes` counts runs in the sorted list, so equal values like `1.50`/`1.5` merge, ties are all kept, output is increasing and each value appears once; empty gives `[]`.
- The four existing functions are unchanged; both new ones are exposed, have examples, and have tests in `stats-test.mo`.

## qya5xxx
yes
- `apply_code` checks the code is known (exact match) before checking whether the cart holds one, so an unknown code is `UnknownCode` either way; a held code gives `AlreadyApplied` with the held code's name.
- `discount` works from the cart's current lines (SAVE10 10% of subtotal, FIVEOFF 5.00, BULK 15% of lines with quantity ≥ 10), rounds once to cents, caps at the subtotal, and is 0 without a code.
- `total` is subtotal less discount; the receipt adds `discount CODE: -AMOUNT` before the total only on a coded cart. A cart without a code behaves as before.
- New names and `CodeError(..)` are exposed, and `cart-test.mo` has tests of the codes.

## qusqybn
yes
- Counts lines exactly (or by `ascii_lowercase` key with `--ignore-case`, keeping the first spelling), and sorts most frequent first with a stable sort over first-appearance order (rules 1–3).
- Pads counts to the width of the largest count actually printed, after `--top` is applied (rule 2), and `--top N` takes the first N, requiring a whole number ≥ 1 (rule 4).
- Options are accepted in either order; anything else, or `--top` without a valid N, prints only the usage line on stderr and exits 2 (rule 5).
- It also treats a repeated option as a usage error. The usage line allows each option once, so that is a fair reading of "anything else".

## qpwnhgh
yes
- `restock.mo` matches the reference's behaviour. Only `count` was written differently: it trims, parses, and rejects negatives and non-numbers as `BadNumber` with the trimmed field.
- Row checks (field count, counts, empty SKU or reorder level not below order-up-to) and messages `line N: ...`, exit 1 for bad rows, and exit 2 for a missing argument or unreadable file all match the task.
- `restock-test.mo` is unchanged; the log ends with a clean `mo check` and passing `mo test`.

## qzr7gmv
yes
- `INCR`/`INCRBY` parse the stored value (missing counts as 0) and the amount as whole numbers, use `checked_add`, and reply `ERR not an integer` or `ERR overflow` leaving the store unchanged; otherwise store and reply the result as text.
- `KEYS` joins the map's keys in insertion order (re-set keeps place, delete then set goes last, tested), and gives `(empty)` for an empty store.
- Wrong word counts (`KEYS a`, `INCR`, `INCRBY a`) fall to `ERR unknown command`; SET/GET/DEL are unchanged and tests of the new commands were added.

## qd9yekx
yes
- `encode` writes each run as length then `Char`, returns `None` on any ASCII digit and `Some("")` for empty text; it has examples and the round-trip promise.
- `decode` builds counts with `checked_mul`/`checked_add`, so an overflowing count is `None`; it rejects a leading 0 (including a count of 0), a character with no digits before it, and trailing digits. Runs of the same character are not merged, and empty text gives `Some("")`.

## qn2bska
yes
- `encode` emits length+character per run of `Char`s, rejects any ASCII digit with `None`, and returns `Some("")` for empty input; it has examples and a round-trip promise.
- `decode` rejects a leading `0`, so `count == 0` reliably means "no digits yet". That makes a character without digits, trailing digits, `0a`, and `03a` all give `None`, and `checked_mul`/`checked_add` catch overflow. Adjacent runs of the same character are allowed.

## qxdevf4
yes
- Trims only spaces and tabs (rule 1). Its own parser accepts exactly `-?digits(.1–18 digits)?` and rejects `3.`, `.5`, `1e3`, and the empty field (rule 2).
- Checks the field count before numbers and names the leftmost bad field. It reports `line N: ...` with physical line numbers (blank lines counted), skips the row, and exits 1 (rule 4).
- Sums exactly in fixed-point `BigInt` (10^-18 units) and rounds once at output to two places, half away from zero (rule 5). It prints `no header` with exit 1 for no input (rule 6).
- It also skips blank lines before the header, a reasonable reading of rule 3, which says such lines are skipped.

## qxjfrmq
yes
- `strip` trims only spaces and tabs (rule 1). `parse_number` accepts `-?digits` with an optional `.` and 1–18 digits and rejects `3.`, `.5`, `1e3`, `-`, `1.2.3`, and empty fields (rule 2).
- Blank lines are skipped and counted. A wrong field count is checked before numbers, and the leftmost bad field is named; bad rows are skipped and the exit is 1 (rules 3–4).
- Exact `BigInt` sums in 10^-18 units, with two-place output rounded half away from zero (rule 5). It prints `no header` with exit 1 for empty input (rule 6).
- Like qxdevf4, it takes the first non-blank line as the header, a reasonable reading of rule 3.

## q26sq2c
yes
- `temperature.mo` now matches the reference's behaviour. Parsing (trimmed, case-insensitive `C`/`F` suffix, decimal number), conversion rounded to one place, `describe`'s `21.5C = 70.7F` line, and `not a temperature: TEXT` with exit 1 are all kept.
- The only difference from the reference is `chars.size` for `char_count`. `temperature-test.mo` is unchanged, and the final check is clean with tests passing.

## qdebfhj
yes
- Strips only spaces and tabs, skips lines that are then empty, and counts every line (rule 1). Numbers must be ASCII digits only and fit in an Int, with leading zeros allowed; LOW ≤ HIGH (rule 2).
- Bad lines are reported as `line N: not a range: TEXT` with the stripped text and exit 1 (rule 3).
- Merging sorts by low and joins overlapping or touching spans, using `saturating_add` to avoid overflow at the Int maximum. Output is `LOW-HIGH` or `N` (rule 4).
- `--gaps` prints the spans between merged ranges; any other arguments print the usage line and exit 2 (rule 5).

## qdfkju3
yes
- `apply_code` matches codes exactly and returns `UnknownCode` before looking at the held code, then `AlreadyApplied` with the held code's name.
- `discount` is computed from the current lines (SAVE10 10% of subtotal, FIVEOFF 5.00, BULK 15% of lines with quantity ≥ 10), rounded once to cents, capped at the subtotal, and 0 without a code.
- `total` is subtotal less discount; the receipt inserts `discount CODE: -AMOUNT` before the total for coded carts only, so uncoded carts are unchanged.
- `CodeError(..)` and the new functions are exposed, and code tests were added to `cart-test.mo`.

## q5ystex
yes
- The fixed `playlist.mo` is identical to the reference solution, so it keeps the program's behaviour: `ARTIST - TITLE (M:SS)` parsing, the `N tracks, M:SS` and `longest: ...` lines, and `skipped: LINE` with exit 1.
- `playlist-test.mo` is unchanged, and the log ends with a clean check and passing tests.

## qzjepsq
yes
- `leap_year?` now applies the full Gregorian rule (divisible by 4, except centuries not divisible by 400), which fixes the failing 1900/2000 test.
- `days_between` was rewritten with a closed form. It counts multiples of 4, 100, and 400 in `[from, to)` using `floor_div`/`floor_mod`, which is correct for every year, negative ones included, and gives 0 when `to <= from`.
- `days_in_month` and `day_of_year` build on `leap_year?` and are right for all years. The tests are unchanged and all pass.

## qqe2ppn
yes
- Apart from the score check, `grades.mo` matches the reference's behaviour. The check, rewritten as a guarded `case`, still rejects non-numbers and scores outside 0–100.
- Averages are rounded to one place, letters are unchanged, `no scores` is kept for an empty list, and bad lines get `line N: can't read LINE` with exit 1.
- `grades-test.mo` is unchanged, and the log ends with a clean check and passing tests.

## q4x6n2j
yes
- The fixed `playlist.mo` differs from the reference only in `parts.get(0)` for `parts.first`, which behaves the same. Track parsing, the summary lines, and `skipped: LINE` with exit 1 are kept.
- `playlist-test.mo` is unchanged, and the final check is clean with tests passing.

## qe99v2f
yes
- The fixed `restock.mo` is the reference solution except for one renamed pattern variable, so behaviour is kept: row parsing and checks, `SKU: order N` lines and the count line, `line N: ...` errors with exit 1, and exit 2 for a missing or unreadable file.
- `restock-test.mo` is unchanged, and the final check is clean with tests passing.

## qfjubyw
yes
- The fixed `rpn.mo` is the reference solution with the `Empty` arm moved within `describe`'s exhaustive `case`, which changes nothing. Evaluation, error positions and messages, `error: ...` output, and exit 1 are kept.
- `rpn-test.mo` is unchanged, and the final check is clean with tests passing.

## qs8q4dp
yes
- The fixed `titles.mo` is identical to the reference solution. The first and last words are always capitalized (`index > 0 and index < last`), small words inside the title are lowercased, and hyphenated parts are capitalized, which holds for every title.
- The tests are unchanged and all pass.

## q34fbvf
yes
- `median` sorts and returns the middle value, or exactly half the sum of the two middle values. It halves via remainders, which is mathematically the same as `(a + b) / 2` and does not round. Empty gives `None`.
- `modes` counts equal decimals under one map key (`1.50` = `1.5`, tested), keeps every value tied for most often once, and sorts them increasing; empty gives `[]`.
- The original four functions are unchanged; both new functions are exposed, have examples, and are tested in `stats-test.mo`.

## qbpbv8a
yes
- The fixed `grades.mo` is the reference solution with `scores.size` bound to a local `count` first, which changes nothing. Parsing (scores 0–100), averages rounded to one place, letters, `no scores`, and `line N: can't read LINE` with exit 1 are kept.
- `grades-test.mo` is unchanged, and the final check is clean with tests passing.

## qqkyhad
yes
- The fixed `calendar.mo` is identical to the reference solution. `leap_year?` applies the full Gregorian rule (every 4th year, except centuries not divisible by 400), so 1900 is not a leap year and 2000 is, for every year.
- The other functions build on `leap_year?`, the tests are unchanged, and all pass.

## qfgd88d
yes
- The fixed `wallet.mo` is identical to the reference solution. `withdraw` refuses only when the amount is greater than the balance, so the whole balance can be withdrawn and the wallet still never goes below zero.
- The tests are unchanged and all pass.

## q53pput
yes
- `INCR`/`INCRBY` share `increment`. It parses the amount and the held value (missing counts as 0) as whole numbers, adds with `checked_add`, and replies `ERR not an integer` or `ERR overflow` with the store unchanged; otherwise it stores and replies the result as text.
- `KEYS` lists keys in insertion order (re-set keeps place, delete then set goes last, tested) or `(empty)`. Wrong word counts reply `ERR unknown command`.
- SET/GET/DEL are unchanged, and thorough tests of the new commands were added to `store.mo`.

## qtzhsm2
yes
- Overlaps: counted shifts are kept per exact name in line order. A shift sharing a minute (strict `<` both ways, so touching is fine) is reported as `line N: overlaps line M` with the earliest such line, is not counted, and sets exit 1.
- `--over HOURS` accepts only ASCII-digit whole numbers ≥ 0 (saturating, so huge values are valid and never exceeded). It prints only totals strictly above, as `NAME: TOTAL (EXTRA over)`.
- Other arguments print the usage line before any input is read and exit 2. Existing totals and `can't read` reporting are kept, and tests of both features were added.

## qevy2g6
yes
- Each person's counted shifts are kept by exact name in line order. A new shift sharing a minute (`a.start < b.finish and b.start < a.finish`, so touching is fine) is reported with the earliest counted overlapping line, is not counted, and sets exit 1.
- `--over HOURS` takes a whole number ≥ 0 and prints only people strictly over, as `NAME: TOTAL (EXTRA over)`; `checked_mul` handles huge limits. Other arguments print the usage line before reading and exit 2.
- Totals and the `can't read` handling are kept, and tests of both features were added.

## qysbujc
yes
- The fixed `search.mo` is identical to the reference solution. `lower_bound` halves over `[0, size)` and returns the size when every element is below `x`, so a number above every element goes at the end. `index_of` and `count_between` are right for every sorted list.
- The tests are unchanged and all pass.

## qb8avb4
yes
- The fixed `parking.mo` is identical to the reference solution. `day_fee` rounds up to each started hour (`(minutes + 59) / 60`) and caps at the daily cap. Stays of at most 30 minutes are free, each full day costs the cap, and negative stays give `None`, per the comments, for every stay.
- The tests are unchanged and all pass.

## qx6er86
yes
- The fixed `search.mo` is identical to the reference solution. Halving `lower_bound` returns the size for a number above every element, and `index_of` and `count_between` are correct for all sorted lists.
- The tests are unchanged and all pass.

## qy5t4zj
yes
- The fixed `rpn.mo` is the reference solution with the `Empty` arm moved within `describe`'s exhaustive `case`, which changes nothing. Evaluation, error messages and positions, and exit 1 are kept.
- `rpn-test.mo` is unchanged, and the final check is clean with tests passing.

## qndjcr4
yes
- Trims only spaces and tabs, skips lines that are then empty, and counts every line (rule 1). Numbers are ASCII digits only and must fit in an Int, with leading zeros allowed; LOW ≤ HIGH (rule 2).
- Bad lines are reported as `line N: not a range: TEXT` with the trimmed text and exit 1 (rule 3). Merging sorts tuples and joins overlapping or touching ones using `saturating_add`, writing `LOW-HIGH` or `N` (rule 4).
- `--gaps` prints the spans between consecutive merged ranges. Anything else prints only the usage line and exits 2 (rule 5).

## qhzsabr
yes
- `edit N TEXT...` checks N with `position` (`no item N`, exit 1, nothing changed), keeps the done flag, joins the words with single spaces as `add` does, and prints `edited N: TEXT`; `edit N` alone falls to usage.
- `clear` keeps undone items in order (so they are renumbered), saves even when none are left so `todo.txt` stays, empty, and prints `cleared K done item(s)` with the singular for 1.
- The usage line is updated, `clear all` is a usage error, the three old commands are unchanged, and tests of both new commands were added.

## q2mh2sm
yes
- The fixed `wallet.mo` is identical to the reference solution. `withdraw` refuses only amounts greater than the balance, so the whole balance can be withdrawn and the wallet never goes below zero.
- The tests are unchanged and all pass.

## qftd6gw
yes
- The fixed `temperature.mo` is identical to the reference solution, keeping parsing, one-place conversion, the `21.5C = 70.7F` line, and `not a temperature: TEXT` with exit 1.
- `temperature-test.mo` is unchanged, and the final check is clean with tests passing.

## q5hsca3
yes
- The fixed `parking.mo` is the reference solution plus two extra examples and a comment. Every started hour is paid (`(minutes + 59) / 60`), capped daily; stays of at most 30 minutes are free; full days cost the cap; negative stays give `None`.
- The tests are unchanged and all pass.

## qymabxn
yes
- A stack of open brackets with line and column (counted in `Char`s, `\n` ends a line) reports the first problem in file order: `unexpected C`, `C does not match O at L:C`, or `O at L:C is never closed` for the innermost one; otherwise it prints `PATH: ok` (rules 1–2).
- An unreadable file goes to stderr as `brackets: <failure>`, as the reference does (`PATH: no such file`), and the program moves on (rule 3).
- Exit is 1 on any problem or read failure and 0 otherwise. No arguments print the usage line with exit 2 (rule 4).

## q4fd7ts
yes
- The fixed `titles.mo` is the reference solution plus one example. The first and last words are always capitalized, small words inside are lowercased, and hyphenated parts are capitalized, which is right for every title.
- The tests are unchanged and all pass.

## qh9urn6
yes
- Lines are counted exactly, or with an `ascii_lowercase` key under `--ignore-case`, keeping the first spelling. A stable descending sort by count keeps first-appearance order for ties (rules 1–3).
- The width comes from the first printed count, which is the largest after `--top`. `--top N` needs ASCII digits ≥ 1 and saturates for huge N, which still prints everything (rules 2, 4).
- Options are accepted in either order. Anything else, a missing or invalid N, or a repeated option prints only the usage line and exits 2; otherwise the exit is 0 (rule 5).

## qn2c7rn
yes
- `edit N TEXT...` needs at least one word, checks N (`no item N` on stderr, exit 1, file unchanged), keeps the done flag, joins the words with single spaces, and prints `edited N: TEXT`.
- `clear` keeps undone items in order, renumbered, saves an empty `todo.txt` when none are left, and prints `cleared K done items` or `cleared 1 done item`.
- The new usage line is used for every other command line (`edit 2`, `clear all`), the old commands are unchanged, and tests of both new commands were added.

## qz4kafb
yes
- `check` walks the characters with a stack of open brackets and their places. Columns count `Char`s, and only `\n` ends a line. It returns the first problem in file order: `unexpected C`, `C does not match O at L:C`, or never-closed for the innermost open bracket (rules 1–2).
- `report` writes the exact formats with PATH as given, or `PATH: ok`. A read failure goes to stderr as `brackets: <failure>` (`PATH: no such file`) and checking continues (rule 3).
- Exit is 1 for any problem or read failure and 0 otherwise. No arguments print the usage line with exit 2 (rule 4).

# Stumbles

For each program: the `mo check` diagnostics in its session log, by code, and what the session did next. ("—" means `mo check` never reported a diagnostic.)

- **quq3hcv**: E0068 (a choice arm is a catch-all, stats.mo:90). Next: replaced the catch-all arm. The next check was clean, tests passed (19, then 28/20 after more edits), and the final check was clean.
- **qya5xxx**: —
- **qusqybn**: —
- **qpwnhgh**: E0023 (expected `in`, found `,`, restock.mo:95). Next: fixed the `for` header. The next check was clean and tests passed (16).
- **qzr7gmv**: —
- **qd9yekx**: —
- **qn2bska**: —
- **qxdevf4**: E1150 (native call not lowered yet, col-sum.mo:29). Next: rewrote that call, then got E1150 again at col-sum.mo:35 and rewrote that one too. The check was then clean and tests passed (1, then 7).
- **q26sq2c**: E0050 (no visible dot-call name, :37), E0070 (case misses `(Fahrenheit, Fahrenheit)`, :52), E0075 (arithmetic on a type without Number, :72), E0051 (visible name bound again, :82). Next: fixed all four in one edit. The check was clean and tests passed (15).
- **qdebfhj**: —
- **qdfkju3**: —
- **q5ystex**: E0024 (no `[...]` indexing, :64), E0006 (character isn't part of Mo, :77). Next: fixed both. The check was clean and tests passed (19, then 12), and the final check was clean.
- **qzjepsq**: — (started from the failing `mo test`, fixed it, and every check was clean).
- **qqe2ppn**: E0025 (`||` → `or`, grades.mo:28). Next: replaced it, which exposed E0068 (catch-all choice arm, :29). Fixed that; the check was clean and tests passed (18).
- **qxjfrmq**: E0075 (arithmetic on a type without Number, :83). Next: changed the arithmetic, then got E1150 (native call not lowered, :6), then E1150 again (:118), working around each. The check was then clean. Tests gave 27 passed and 3 stopped, which it fixed to 30 passed.
- **q4x6n2j**: E0024 (no `[...]` indexing, :64), E0006 (character isn't part of Mo, :77). Next: the fix introduced E0050 (no visible dot-call name, :58 and :94). Corrected those names; the check was clean and tests passed (19).
- **qe99v2f**: E0023 (expected `in`, found `,`, :95). Next: the fix exposed E0086 (`try` needs the same failure kind, :19), E0056 (result isn't used, :69), E0070 (case misses `BadLevels(..)`, :74), and E0053 (values of different types combined, :101). Fixed all four; the check was clean and tests passed (16).
- **qfjubyw**: E0053 (:25), E0050 (:44), E0068 (catch-all arm, :72), E0077 (argument type doesn't match parameter, :80). Next: fixed the first two, with E0068 and E0077 remaining. Fixed those; the check was clean and tests passed (16).
- **qs8q4dp**: — (started from the failing `mo test`, fixed it, and the check was clean).
- **q34fbvf**: — (one test run stopped and was fixed; every check was clean).
- **qbpbv8a**: E0025 (`||` → `or`, :28). Next: the edit gave E0053 (:28), E0050 (name not visible, :42), E0075 (:76), and E0051 (:84). It ran `mo test`, which showed the same four diagnostics and ran no tests, then fixed them all. The check was clean and tests passed (18).
- **qqkyhad**: — (started from the failing `mo test`, fixed it, and the check was clean).
- **qfgd88d**: — (started from the failing `mo test`, fixed it, and the check was clean).
- **q53pput**: —
- **qtzhsm2**: E1103 (body rule not implemented by the staged checker, timesheet.mo:72). Next: rewrote that code. The check was clean and tests passed (11, 16, 18 as tests were added).
- **qysbujc**: — (started from the failing `mo test`, fixed it, and the check was clean).
- **qb8avb4**: — (started from the failing `mo test`, fixed it, and the check was clean).
- **qx6er86**: — (started from the failing `mo test`, fixed it, and the check was clean).
- **qy5t4zj**: E0053 (:25), E0050 (:44), E0068 (catch-all arm, :72), E0077 (:80). Next: fixed all four in one edit. The check was clean and tests passed (16).
- **qevy2g6**: E1103 (body rule not implemented by the staged checker, timesheet.mo:148). Next: rewrote that code. The check was clean and tests passed (28, 34). A later run had 1 failing test, which it fixed (35 passed).
- **qndjcr4**: —
- **qhzsabr**: —
- **q2mh2sm**: — (started from the failing `mo test`, fixed it, and the check was clean).
- **qftd6gw**: E0050 (:37), E0070 (case misses `(Fahrenheit, Fahrenheit)`, :52), E0075 (:72), E0051 (:82). Next: fixed the first two, with E0075 and E0051 remaining. Fixed those; the check was clean and tests passed (15).
- **q5hsca3**: — (started from the failing `mo test`, fixed it, and the check was clean).
- **qymabxn**: —
- **q4fd7ts**: — (started from the failing `mo test`, fixed it, and the check was clean).
- **qh9urn6**: —
- **qn2c7rn**: — (an early `mo test` had 2 failures, which it fixed; every check was clean).
- **qz4kafb**: —
