# Grades (grader A)

## q75m3kc
yes
Adds `CodeError` with both cases, `apply_code` checks unknown codes before the held code, and matches exactly.
`discount` is computed from current lines, rounded once with `round(2)`, and capped at the subtotal; 0 without a code.
`total` and `receipt` (discount line just before the total, two decimals) follow the task; the old behaviour is kept for a cart without a code.
New names are exposed and `cart-test.mo` gains tests of all three codes, the errors, rounding and the cap.

## qpxwfdu
yes
`playlist.mo` now matches a version that checks cleanly; its session's `mo check` ends with no diagnostics and `mo test` passes every test.
Behaviour is kept: `ARTIST - TITLE (M:SS)` parsing, the `N tracks, TOTAL` and `longest: ...` lines, `skipped: LINE` on stderr with exit status 1.
`playlist-test.mo` is unchanged, as the task requires.

## qwhgz86
yes
Codes are stored as a private `Code` enum and parsed by exact text match, so `save10` gives `UnknownCode`; an unknown code is rejected before the held code is checked (rule 2).
`discount` sums from the current lines (BULK: 15% of lines with quantity ≥ 10), rounds once with `round(2)`, caps at the subtotal, and is 0 without a code (rule 3).
`total` subtracts the discount; `receipt` puts `discount CODE: -AMOUNT` just before the total only when a code is held (rule 4).
`CodeError(..)` is exposed and the tests cover each code, rounding, the cap, later additions, exact matching and the one-code limit.

## q5t8pfw
yes
`encode` gives `None` for any ASCII digit, `Some("")` for the empty text, and writes each run of `Char`s as length then character, Unicode and whitespace included.
`decode` rejects a leading 0 or count of 0 (check on the first digit), a character with no digits, trailing digits, and overflow (`checked_mul`/`checked_add`); runs of the same character are not merged.
Both functions have examples, and `encode` has a promise that decoding its result gives the text back.

## q7jk7e3
yes
Overlaps: each person's counted shifts are kept in line order, so `find` gives the earliest counted line that shares a minute; touching shifts don't overlap; names are compared exactly; an overlapping shift is not counted and sets status 1.
`--over HOURS` accepts only all-digit HOURS (BigInt, so no overflow), prints only totals strictly over the limit as `NAME: TOTAL (EXTRA over)`; any other arguments print the usage line, read nothing, and return 2.
Existing totals, ordering and `can't read` reporting are unchanged, and tests of both extensions are added to `timesheet.mo`.

## qweqrfq
yes
`grades.mo` is the reference program except for naming `scores.size` as `count` before dividing, which doesn't change behaviour.
The session's last `mo check` reports no diagnostics and `mo test` passes 18 of 18; `grades-test.mo` is unchanged.
Parsing `NAME: s, s, ...`, average to one decimal, letter grade and `line N: can't read LINE` with status 1 are kept.

## qb5h4h4
yes
Counts each distinct line (exact, or by `ascii_lowercase` key with `--ignore-case`, keeping the first spelling), sorted most frequent first with a stable sort, so equal counts keep first-seen order (rules 1–3).
Counts are padded to the widest count among the lines printed after `--top` (rule 2, 4); `--top` needs N ≥ 1.
Options parse in either order; anything else gives the usage line on stderr, no output, and status 2 (rule 5). Repeating an option is treated as a usage error, which is a defensible reading of "anything else".

## q77skya
yes
`lower_bound` is a correct halving search: `middle` is always within `[low, high)`, so the `stop` branch can't be reached, and a number above every element gives `items.size`.
`index_of` and `count_between` rest on it and are right for every sorted list; the search still halves.
Tests are unchanged and the session's `mo test` passes all of them.

## qewu5k8
yes
`check` keeps a stack of open brackets with their line and character column, and returns the first problem: unexpected closer, mismatch (both positions), or the innermost bracket left open (rules 1–2).
Every other character is ignored; `\n` starts a new line and resets the column.
`main` prints one line per readable file in order, reports a missing file as `brackets: PATH: no such file` and goes on, exits 1 on any problem or unreadable file, and with no arguments prints the usage line and exits 2 (rules 3–4).

## q58mueh
yes
`withdraw` now refuses only an amount greater than the balance, so the whole balance can be withdrawn and the balance can never go below zero.
The whole-cents check is written as `amount % 0.01 != 0`, which gives the same results as the reference's `round(2)` test for exact decimals; a promise that the balance stays ≥ 0 is added.
Tests are unchanged and the session's last `mo test` passes all of them.

## qtqhdxn
yes
`wallet.mo` matches the reference fix: the overdraw check rejects only `amount > balance`, so the whole balance can be withdrawn and the balance never goes below zero.
Tests are unchanged and the session's `mo test` passes 11 of 11.

## quqw5s5
yes
`titles.mo` matches the reference fix: a small word is lowercased only when it is neither the first nor the last word, so the first word is always capitalized.
This holds for every title, hyphenated parts and repeated spaces included; tests are unchanged and the session's `mo test` passes 12 of 12.

## qmpksh8
yes
Lines are counted exactly or by `ascii_lowercase` key keeping the first spelling, then sorted by count with a stable sort, so ties keep first-seen order (rules 1–3).
`--top N` takes all-digit N ≥ 1 (saturating, so huge N prints everything); the width is the count of the first line printed, the largest shown (rules 2, 4).
Options go in either order; anything else (repeats included) gives only the usage line and status 2 (rule 5).

## qn29hjk
yes
`edit N TEXT...` needs at least one word, joins words with single spaces, keeps the done state, prints `edited N: TEXT`, and reports a bad N as `no item N` with status 1 without saving.
`clear` keeps undone items in order (so they are numbered again from 1), saves an empty `todo.txt` when none are left, and prints `cleared K done item(s)` with the singular for 1.
Other command lines (`edit 2`, `clear all`) give the new usage line with status 1; `add`, `list` and `done` are unchanged, and tests of both commands are added.

## qf4gjuu
yes
A stack of openers with line and character column gives the first problem in reading order: `unexpected C`, `C does not match O at L:C`, or the innermost opener left open (rules 1–2).
Columns count `Char`s and reset after `\n`; every other character, quotes and comments included, is ignored.
Files are handled in order; a missing one goes to stderr as `brackets: PATH: no such file` and the next is checked; status is 0/1, and with no arguments it prints the usage line and exits 2 (rules 3–4).

## qnsmybh
yes
`leap_year?` now applies the 100 and 400 rules (`year % 4 == 0 and (year % 100 != 0 or year % 400 == 0)`), so 1900 is not a leap year and 2000 is.
`days_between` was rewritten as a closed form, 365 a year plus multiples of 4 − 100 + 400 in `[from, to)` using floor-based ceiling division. This is right for every year, negative years included, and gives 0 when `to <= from` like the loop it replaced.
Tests are unchanged and pass in the session.

## qewhp8b
yes
`calendar.mo` matches the reference fix: `leap_year?` applies the 100 and 400 rules, which also makes `days_in_month`, `day_of_year` and `days_between` right for every year.
Tests are unchanged and the session's `mo test` passes 17 of 17.

## qd8crva
yes
Trims only spaces and tabs, skips empty lines, and accepts `N` or `LOW-HIGH` of ASCII digits that fit an Int with LOW ≤ HIGH; any other line gives `line N: not a range: TEXT` and status 1, with N counting every line (rules 1–3).
Sorted spans are merged when overlapping or touching (`saturating_add` avoids overflow at the top), printed as `LOW-HIGH` or `N` (rule 4).
`--gaps` prints only the spans between merged ranges; any other arguments give the usage line and status 2 before reading (rule 5).

## q4gp7kv
yes
`strip` trims only spaces and tabs; empty lines are skipped but still counted; `N` / `LOW-HIGH` need ASCII digits that fit an Int with LOW ≤ HIGH; other lines give `line N: not a range: TEXT` and status 1 (rules 1–3).
`merge` sorts and joins overlapping or touching spans (overflow-safe at the top), and the output uses `LOW-HIGH` or `N` (rule 4).
`--gaps` prints the spans between merged ranges; any other argument list prints only the usage line and returns 2 (rule 5).

## q6723jz
yes
`median` sorts, takes the middle value or the exact mean of the two middle ones (halving each part separately, so the sum can't overflow, still exact within Decimal), and gives `None` for an empty list (rule 1).
`modes` groups equal values (so `1.50` and `1.5` are one), keeps every value tied for the top count once, and sorts them; empty gives `[]` (rule 2).
Both are exposed and have examples; the four existing functions are unchanged; tests of both are added to `stats-test.mo`.

## q55nkhs
yes
`titles.mo` is the reference fix plus one extra example (`"of mice and men"` → `"Of Mice and Men"`): small words are lowercased only inside the title, so the first word is always capitalized.
Right for every title; tests are unchanged and pass in the session.

## qaxbzaj
yes
Overlaps: each person's counted shifts are kept in line order and searched with `find`, so the earliest overlapping counted line is named; touching shifts don't overlap, names are exact, and an uncounted shift doesn't block later ones (rule 1).
`--over HOURS` takes all-digit HOURS (huge values are clamped, so nobody is over), prints only totals strictly over as `NAME: TOTAL (EXTRA over)`; other arguments print the usage line, read nothing, and return 2 (rule 2).
Existing behaviour is kept and tests of both are added.

## qa7rv5w
yes
`rpn.mo` is the reference fix except that the `Empty` arm of `describe` comes last instead of first, which doesn't change behaviour since the arms are exhaustive and disjoint.
The session's `mo check` reports no diagnostics and `mo test` passes 16 of 16; tests are unchanged; evaluation, `error: ...` messages and status 1 are kept.

## qmfdju5
yes
`grades.mo` is the reference fix except that the 0–100 range check is written as a guarded `case`, which rejects the same scores.
The session's last `mo check` is clean and `mo test` passes; tests are unchanged; averages, letters and `line N: can't read LINE` with status 1 are kept.

## qweqkfh
yes
`parking.mo` matches the reference fix: `day_fee` charges each started hour (`(minutes + 59) / 60`) up to the daily cap; the only other changes are extra examples (`day_fee(1)`, `day_fee(61)`, `fee(31)`, `fee(1441)`) that agree with the rules.
Fees follow the comments for every stay; tests are unchanged and pass.

## qr2dgzc
yes
`search.mo` matches the reference fix: `lower_bound` searches by halving over `[0, items.size)` and gives `items.size` for a number above every element; `index_of` and `count_between` are right for every sorted list.
Tests are unchanged and pass.

## q7trytz
yes
`restock.mo` is the reference fix except that the count check is written as a guarded `case` on `to_int`, which accepts and rejects the same fields with the same `BadNumber` error.
The session's `mo check` is clean and `mo test` passes; tests are unchanged; the CSV reading, order lines, row errors with status 1 and status 2 cases are kept.

## qx6386t
yes
`INCR` and `INCRBY` add to the value (a missing key counts as 0) with `to_int` and `checked_add`, store and reply the result as text, and reply `ERR not an integer` / `ERR overflow` with the store unchanged (rules 1–2, 4).
`KEYS` joins the map's keys, which keep insertion order (a reset keeps its place; delete-then-set goes last), or replies `(empty)` (rule 3).
Exact word counts still fall through to `ERR unknown command`; the three old commands are unchanged; tests of the new commands are added.

## qstg5mw
yes
`playlist.mo` is the reference fix except that the artist is taken with `try parts.get(0)` instead of `parts.first or ""`; `parts.size >= 2` is checked just before, so the behaviour is the same.
The session's last `mo check` is clean and `mo test` passes 19 of 19; tests are unchanged.

## qbux7bk
yes
`edit` and `clear` match the reference logic: edit needs words, keeps the done state, prints `edited N: TEXT`, and reports a bad N as `no item N` (status 1, nothing saved); clear keeps undone items in order, saves an empty file when none are left, and uses `item`/`items` correctly.
The usage line is updated, `edit 2` / `clear all` give it, and the three old commands are unchanged; tests of both new commands are added.

## qp3rqa8
yes
`restock.mo` is the reference fix except for a renamed pattern variable in `describe` (`sku` for `s`), which doesn't change behaviour.
The session's last `mo check` is clean and `mo test` passes 16 of 16; tests are unchanged.

## qkurx3c
yes
Fields are trimmed of spaces and tabs and parsed by hand as `-?DIGITS(.1–18 DIGITS)?`, so `3.`, `.5`, `1e3` and `""` are rejected; the count check comes before the leftmost bad field; lines are numbered from 1 with blank lines counted and skipped (rules 1–4).
Totals are kept exactly as BigInt units of 10⁻¹⁸ and written with two places, a half rounded away from zero; empty columns give `0.00` (rule 5).
No input gives `no header` and status 1 (rule 6). The header is taken as the first non-blank line, which applies the blank-line rule to it too; that is a defensible reading.

## qa4f9uj
yes
`INCR`/`INCRBY` parse the held value (missing = 0) and AMOUNT with `to_int`, add with `checked_add`, store and reply the text, and give `ERR not an integer` / `ERR overflow` with the store unchanged (rules 1–2, 4).
`KEYS` lists the map's keys in insertion order, or `(empty)` (rule 3); wrong word counts give `ERR unknown command`.
The old commands are unchanged and tests of the new ones are added (examples plus a test of counters and key order).

## qkz4rz5
yes
`temperature.mo` is the reference fix except that `try ... to_decimal` is written out as a `case` with the same result.
The session's `mo check` is clean and `mo test` passes; tests are unchanged; conversion, rounding to one place, case-insensitive C/F and `not a temperature: TEXT` with status 1 are kept.

## qfcxv8y
yes
`median` sorts and returns the middle value, or for an even count `low + (high - low) / 2` when the signs agree (to avoid overflow) and `(low + high) / 2` otherwise, both exact; `None` when empty (rule 1).
`modes` groups equal decimals, keeps each value with the top count once, and sorts them; empty gives `[]` (rule 2).
The four existing functions are byte-identical, both new ones are exposed and have examples, and tests of both are added.

## qfdg3uh
yes
`temperature.mo` matches the reference fix; the session's last `mo check` is clean and `mo test` passes 15 of 15, with the tests unchanged.

## qjrhm93
yes
`encode` refuses any ASCII digit, gives `Some("")` for the empty text, and writes each run of a `Char` as count then character, whitespace and Unicode included.
`decode` tracks an optional count: a leading 0 is refused, a character with no count, trailing digits and overflow (`checked_mul`/`checked_add`) give `None`, and runs aren't merged.
Both have examples, and `encode` has the round-trip promise.

## qvjajzh
yes
`rpn.mo` is the reference fix with the `Empty` arm of `describe` moved last, which doesn't change behaviour; the session's `mo check` is clean and `mo test` passes 16 of 16, with the tests unchanged.

## qf36rhp
yes
The header is the first line; fields are trimmed of spaces and tabs only and parsed by hand (optional `-`, digits, optional `.` and 1–18 digits), so `3.`, `.5`, `1e3` and `""` are rejected (rules 1–2).
Blank lines are skipped but counted; the field-count error comes before the leftmost non-number, and a bad row is skipped with status 1 (rules 3–4).
Sums are exact BigInt units of 10⁻¹⁸, rounded once to cents with halves away from zero (rule 5); no input gives `no header` and status 1 (rule 6).

## q3ynvfg
yes
`parking.mo` matches the reference fix: `day_fee` charges each started hour (`(minutes + 59) / 60`) at 2.50, capped at 20.00 a day, so fees follow the rules for every stay.
Tests are unchanged and the session's `mo test` passes 13 of 13.

## Stumbles

The `mo check` diagnostics each session logged, by code, and what it did next. The logs record runs only, not edits, so "edited" means the next run came back with a different result. In the "fix compile errors" tasks (playlist, grades, rpn, restock, temperature), the first run's diagnostics belong to the program as it was handed over.

- q75m3kc: E0046 (cart-test.mo:3, imported name doesn't exist), on the third run after adding tests. Next it edited and reran `mo check`, which was clean, then `mo test` (8 passed).
- qpxwfdu: E0024 (indexing with `[...]`), E0006 (character not part of Mo), from the given program. Next `mo check` was clean, then `mo test` passed.
- qwhgz86: none.
- q5t8pfw: none.
- q7jk7e3: none. It had two failing `mo test` runs (runs 4 and 8), each fixed on the next run.
- qweqrfq: E0025 (`||` for `or`), from the given program. Next it ran `mo fix` (no diagnostics), but `mo check` still gave E0025. The next check gave E0053 (different types combined), E0050 (name not visible), E0075 (arithmetic without Number), E0051 (name bound again). After a further edit `mo check` was clean and `mo test` passed.
- qb5h4h4: none.
- q77skya: none.
- qewu5k8: E0007 (expected line break, found `if`). Next `mo check` was clean, then `mo test` passed 21.
- q58mueh: none. Its `mo test` had a stopped test on runs 5–6; the same run repeated once, then it edited and got 12 passed.
- qtqhdxn: none.
- quqw5s5: none.
- qmpksh8: none. One `mo test` failed 4 tests and was fixed on the next run.
- qn29hjk: none. One `mo test` failed 2 tests and was fixed on the next run.
- qf4gjuu: none.
- qnsmybh: none.
- qewhp8b: none.
- qd8crva: none.
- q4gp7kv: E0021 (expected `end`, found `)`), at ranges.mo:92 in a promise it was adding after tests passed. Next `mo check` was clean and `mo test` passed 32.
- q6723jz: none. One `mo test` had a stopped test, fixed on the next runs.
- q55nkhs: none.
- qaxbzaj: none.
- qa7rv5w: E0053, E0050 (no dot-call function of that name), E0068 (choice arm is a catch-all), E0077 (argument type mismatch), from the given program. Next `mo check` was clean, then `mo test` passed 16.
- qmfdju5: E0068 (choice arm is a catch-all), from the given program. Next `mo check` was clean, then `mo test` passed.
- qweqkfh: none.
- qr2dgzc: none.
- q7trytz: E0023 (expected `in`, found `,`), from the given program. Next `mo check` was clean, then `mo test` passed.
- qx6386t: none.
- qstg5mw: E0024, E0006, from the given program. Next `mo check` gave E0050 (no dot-call function of that name, playlist.mo:58). After another edit `mo check` was clean and `mo test` passed 19.
- qbux7bk: none.
- qp3rqa8: E0023, from the given program. Next `mo check` gave E0086 (`try` needs the same failure kind), E0056 (result not used), E0070 (case not exhaustive, missing `BadLevels(..)`), E0053. After another edit `mo check` was clean and `mo test` passed 16.
- qkurx3c: none.
- qa4f9uj: none.
- qkz4rz5: E0050, E0070 (missing `(Fahrenheit, Fahrenheit)`), E0095 (`x.f()` for `x.f`), E0051, from the given program. Next `mo check` was clean, then `mo test` passed.
- qfcxv8y: none.
- qfdg3uh: E0050, E0070, E0095, E0051, from the given program. Next `mo check` gave E0077 (argument type mismatch) and E0075 (arithmetic without Number). After another edit `mo check` was clean and `mo test` passed 15.
- qjrhm93: none.
- qvjajzh: E0053, E0050, E0068, E0077, from the given program. Next `mo check` was clean, then `mo test` passed 16.
- qf36rhp: none.
- q3ynvfg: none.
