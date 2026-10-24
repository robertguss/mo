## qd9yekx
yes
`encode` handles runs of `Char`, rejects ASCII digits, and includes examples and the required round-trip promise.
`decode` rejects missing counts/characters, zero and leading-zero counts, and checked-arithmetic overflow; both functions handle empty text.

## qzr7gmv
yes
The original commands remain intact; increments parse signed integers, check overflow, and leave the store unchanged on errors.
`KEYS` uses insertion order, including delete-and-reinsert behavior; command arities and tests cover the new commands.

## qe99v2f
yes
The corrected program preserves row validation, reorder calculations, output, error messages, and exit statuses, without changing the supplied tests.
The session log records a clean final check and all 16 tests passing.

## qfjubyw
yes
The corrected evaluator preserves operand order, integer division, token-position errors, per-line output, and failure exit status; the supplied tests are unchanged.
The session log records a clean final check and all 16 tests passing.

## qx6er86
yes
Using `items.size` as the exclusive upper bound permits insertion after the last element while retaining binary search.
`index_of` finds the first equal element and `count_between` implements the required half-open interval; supplied tests are unchanged.

## qfgd88d
yes
Withdrawals reject only amounts greater than the balance, allowing an exact withdrawal to leave zero.
Positive-whole-cent validation and transfer failure propagation remain intact; the supplied tests are unchanged and the log reports all 11 passing.

## qqe2ppn
yes
Parsing validates names and scores, averaging uses decimal division and one-place rounding, and reporting preserves grade bands and line-numbered errors.
The supplied tests are unchanged; the session records clean checks and all 18 tests passing.

## qn2c7rn
yes
`edit` validates the item number before saving, joins the replacement words, and preserves the done flag.
`clear` preserves unfinished-item order and saves an empty file when necessary; usage, error status, singular/plural replies, and tests meet the task.

## qzjepsq
yes
The Gregorian divisibility rule handles century exceptions generally, and all month/date functions use it.
`days_between` counts leap-year multiples over the requested interval, including negative years; the supplied tests are unchanged and the log records passing runs.

## q53pput
yes
The new increments use strict integer parsing and checked addition, leaving the store unchanged on either error.
Original commands and arities are preserved, `KEYS` follows insertion order, and the added tests exercise updates, reinsertion, and boundary cases.

## qy5t4zj
yes
The evaluator correctly pops the right operand before the left, propagates evaluation errors, and reports token positions and per-line status.
The supplied tests are unchanged; the final checks are clean and both later test runs report all 16 passing.

## qz4kafb
yes
The bracket stack finds the first unexpected or mismatched closing bracket, or the innermost opening bracket left at EOF, ignoring all other characters.
Character-based positions, exact reports, file ordering, unreadable-file handling, and exit statuses satisfy the task's four bullets.

## q4fd7ts
yes
Small words stay lowercase only inside the title, so the first and last words are always capitalized.
ASCII capitalization, hyphenated parts, and whitespace collapsing remain correct; the supplied tests are unchanged.

## qqkyhad
yes
The leap-year predicate implements the general divisible-by-4, century, and divisible-by-400 rules.
Month lengths, valid dates, and year totals inherit the correction; the supplied tests are unchanged and all 17 tests pass in the log.

## qdfkju3
yes
Exact code matching and unknown-code precedence are correct, and adding lines preserves the applied code.
Discounts use current lines, combine before rounding, cap at the subtotal, and appear immediately before receipt totals; the new names and tests are present.

## qdebfhj
yes
Parsing enforces ASCII digits, 64-bit bounds, ordered endpoints, and space/tab-only trimming; all physical input lines are counted.
Sorting and overflow-safe adjacency checks produce minimal merged ranges, and `--gaps`, diagnostics, output forms, and usage handling meet the five bullets.

## q2mh2sm
yes
An amount equal to the balance now succeeds, while larger withdrawals still fail before subtraction.
Positive-whole-cent validation and transfers remain correct; the supplied tests are unchanged and the log records all 11 passing.

## qevy2g6
yes
Only counted shifts of the exact same name are checked, in input order, using strict interval overlap, so touching shifts remain valid.
`--over` filters strictly above the threshold, formats the excess correctly, handles large thresholds without multiplication overflow, and rejects bad arguments before reading input; tests cover both features.

## qs8q4dp
yes
The small-word exception now applies only between the first and last words, while ASCII and hyphen-part capitalization and collapsed whitespace remain intact.
The supplied tests are unchanged, and the log records a clean check followed by all 12 tests passing.

## qxdevf4
no
The first-line-header rule is violated: `main` skips blank lines before choosing a header.
For input `\n1\n`, it treats `1` as the header and prints `1: 0.00`, instead of totaling `1` under the empty name from the first line (`: 1.00`).
A single blank header line also incorrectly produces `no header` and status 1, although input exists.

## qya5xxx
yes
The public API, exact code matching, unknown-code precedence, and one-code limit satisfy the task.
Current-line discounts are summed before rounding, capped at the subtotal, preserved across additions, and printed before receipt totals; code tests are included.

## qysbujc
yes
The exclusive upper bound is the list size, so binary search handles insertion at the end as well as empty lists and duplicate values.
The other functions correctly derive first-match lookup and half-open interval counts from that bound; the supplied tests are unchanged.

## qh9urn6
yes
Counts preserve exact lines and first spelling, use ASCII-only case folding when requested, and rank ties in first-appearance order.
Output padding, either option order, positive `--top` limits, usage errors, and exit statuses satisfy the task's five bullets.

## quq3hcv
yes
`median` selects the sorted middle value or computes the two-middle-value mean without explicit rounding; empty input returns `None`.
`modes` groups numerically equal sorted values and returns every maximum-frequency value once in increasing order; the original functions, examples, and tests are preserved or extended as required.

## qusqybn
yes
Exact-line tallies retain first spelling and insertion order; stable descending-count sorting preserves ties, and padding uses the counts actually printed.
ASCII case folding, top selection, either option order, usage handling, and exit statuses follow the task.

## q5ystex
yes
The repaired program preserves duration and track parsing, first-longest tie selection, summary formatting, skipped-line diagnostics, and failure status.
The supplied tests are unchanged; the session records clean checks and passing test runs.

## q34fbvf
yes
`median` sorts and selects the correct middle values, averaging an even pair without explicit rounding and avoiding overflow in their direct sum.
`modes` counts equal decimal keys together and returns all most-frequent keys sorted; original functions remain intact and examples and tests are present.

## qftd6gw
yes
Parsing accepts either case of the two scales, conversions use the correct formulas and one-place rounding, and descriptions convert to the opposite scale.
Invalid arguments retain the required stderr message and status; the supplied tests are unchanged, with a clean final check and all 15 tests passing in the log.

## qb8avb4
yes
Rounding each partial day's paid hours upward fixes started-hour charges without subtracting the initial free allowance from longer stays.
The overall 30-minute free stay, separate daily caps, negative-stay rejection, and receipt formatting remain correct; supplied tests are unchanged.

## qhzsabr
yes
`edit` preserves completion state, validates numbers before writing, joins text with spaces, and returns the required success or error message.
`clear` removes only done items, preserves order, retains an empty file, and uses correct singular/plural replies; updated usage and tests cover the additions.

## qxjfrmq
no
The header loop skips blank lines, contradicting the rule that the first input line is the header.
For input `\n1\n`, the program prints `1: 0.00` rather than `: 1.00`; for a single blank header line it incorrectly reports `no header` with status 1.
Its exact arithmetic and row validation do not fix that input-handling mismatch.

## qndjcr4
yes
Parsing accepts only bounded ASCII whole numbers and ordered ranges, trims only spaces/tabs, and reports invalid physical lines without counting them toward output.
Sorted merging handles overlap and adjacency without overflowing; gaps, singleton formatting, usage errors, and statuses meet the task.

## qpwnhgh
yes
The repaired program validates all fields and levels, orders at or below the reorder threshold, and preserves summaries, row diagnostics, and exit statuses.
The supplied tests are unchanged; the session records clean checks and passing test runs.

## q26sq2c
yes
The corrected program preserves case-insensitive scale parsing, conversion formulas, one-place rounding, opposite-scale descriptions, and invalid-reading diagnostics.
The supplied tests are unchanged; both later test runs report all 15 passing and both later checks are clean.

## qymabxn
yes
The stack enforces innermost matching, reports the first closing-bracket error or innermost unclosed opener, and counts Unicode characters rather than bytes.
Paths, exact messages, stdout/stderr routing, continued checking after read failures, and exit statuses satisfy the task's four bullets.

## qn2bska
yes
Encoding counts `Char` runs, rejects ASCII digits, and supplies examples and the round-trip promise.
Decoding checks count overflow, rejects zero/leading-zero counts and incomplete runs, permits adjacent runs of the same character, and handles empty text.

## q5hsca3
yes
The ceiling-hour calculation charges every started hour, while the whole-stay free threshold and independent daily caps remain correct.
Negative stays and receipt formatting are preserved; the supplied tests are unchanged and the log records a clean check and all 15 tests passing.

## qtzhsm2
yes
Per-person counted shifts retain input order, and strict interval checks report the earliest counted overlapping line without rejecting touching shifts.
`--over` validates before reading, compares strictly, prints the correct excess, and handles very large thresholds safely; both new features have tests.

## qbpbv8a
yes
The repaired program preserves score validation, exact decimal averaging followed by one-place rounding, grade bands, no-score reports, and line-numbered errors.
The supplied tests are unchanged; the final check is clean and the final test run reports all 18 passing.

## q4x6n2j
yes
The repaired program preserves duration and track parsing, longest-track tie order, summary formatting, stderr diagnostics, and failure status.
The supplied tests are unchanged; the final check is clean and all 19 tests pass in the log.

## Stumbles list

These are the recorded session runs, not runs performed for this grading. The supplied logs show diagnostics and subsequent checks/tests, but not intermediate edits; the follow-up descriptions below report only what the logs establish. Test totals are written as passed/failed/stopped.

- **qd9yekx:** No `mo check` diagnostics (001). Next: `mo test` (002), 21/0/0.
- **qzr7gmv:** No `mo check` diagnostics (001). Next: `mo test` (002), 20/0/0.
- **qe99v2f:** Check 001: `E0023` at `restock.mo:95`, expected `in`, found comma. Next check (002): `E0086` at :19, incompatible `try` failure kind; `E0056` at :69, unused result; `E0070` at :74, missing `BadLevels(..)` case; `E0053` at :101, combined different types. Next check (003) was clean, then test 004 was 16/0/0.
- **qfjubyw:** Check 001: `E0053` at `rpn.mo:25`, combined different types; `E0050` at :44, no visible dot-call function; `E0068` at :72, catch-all choice arm; `E0077` at :80, argument/parameter type mismatch. Next check (002) retained `E0068` and `E0077` at the same locations. Check 003 was clean; test 004 was 16/0/0.
- **qx6er86:** No `mo check` diagnostics (002). An initial test (001) was 11/1/0; after the clean check, tests 003–005 were 12/0/0, 8/0/0, and 8/0/0.
- **qfgd88d:** No `mo check` diagnostics (002). Initial test 001 was 10/1/0; next after the clean check, test 003 was 11/0/0.
- **qqe2ppn:** Check 001: `E0025` at `grades.mo:28`, use `or` instead of `||`. Next check (002): `E0068` at :29, catch-all choice arm. Check 003 was clean, test 004 was 18/0/0, and check 005 remained clean.
- **qn2c7rn:** No `mo check` diagnostics (002, 004). Initial test 001 was 8/2/0; after check 002, test 003 was 10/0/0; after check 004, test 005 was 15/0/0.
- **qzjepsq:** No `mo check` diagnostics (002). Initial test 001 was 16/1/0; after the clean check, tests 003–005 were 21/0/0, 16/0/0, and 21/0/0.
- **q53pput:** No `mo check` diagnostics (001, 003). Each was followed by a passing test run: 002 was 8/0/0 and 004 was 12/0/0.
- **qy5t4zj:** Check 001: `E0053` at `rpn.mo:25`, combined different types; `E0050` at :44, no visible dot-call function; `E0068` at :72, catch-all choice arm; `E0077` at :80, argument/parameter type mismatch. Next check (002) was clean, test 003 was 16/0/0, check 004 was clean, and test 005 was 16/0/0.
- **qz4kafb:** No `mo check` diagnostics (001, 003). Next after check 001: test 002, 22/0/0; the session then ended with clean check 003.
- **q4fd7ts:** No `mo check` diagnostics (002). Initial test 001 was 10/2/0; after the clean check, test 003 was 13/0/0.
- **qqkyhad:** No `mo check` diagnostics (002). Initial test 001 was 16/1/0; after the clean check, test 003 was 17/0/0.
- **qdfkju3:** No `mo check` diagnostics (001, 003, 005). Each was followed by tests: 002 was 6/0/0, 004 was 6/0/0, and 006 was 15/0/0.
- **qdebfhj:** No `mo check` diagnostics (001, 003). Test 002 after the first check was 35/0/0; tests 004–005 after the second check were each 36/0/0.
- **q2mh2sm:** No `mo check` diagnostics (002). Initial test 001 was 10/1/0; after the clean check, test 003 was 11/0/0.
- **qevy2g6:** Check 001: `E1103` at `timesheet.mo:148`, body rule not implemented by the staged checker. Next check (002) was clean; test 003 was 28/0/0. Checks 004 and 006 were clean, followed respectively by tests 005 (34/0/0) and 007 (34/1/0). Check 008 was clean and test 009 was 35/0/0.
- **qs8q4dp:** No `mo check` diagnostics (002). Initial test 001 was 10/2/0; after the clean check, test 003 was 12/0/0.
- **qxdevf4:** Check 001: `E1150` at `col-sum.mo:29`, native call not lowered. Next check (002) still reported `E1150`, now at :35. Check 003 was clean, test 004 was 1/0/0, check 005 was clean, and test 006 was 7/0/0.
- **qya5xxx:** No `mo check` diagnostics (001, 003). Each was followed by tests: 002 was 5/0/0 and 004 was 18/0/0.
- **qysbujc:** No `mo check` diagnostics (002, 004). Initial test 001 was 11/1/0; after check 002, test 003 was 12/0/0; after check 004, test 005 was 9/0/0.
- **qh9urn6:** No `mo check` diagnostics (001, 003). Each was followed by tests: 002 was 1/0/0 and 004 was 8/0/0.
- **quq3hcv:** Check 001: `E0068` at `stats.mo:90`, catch-all choice arm. Next check (002) was clean and test 003 was 19/0/0. Check 004 was clean, tests 005–006 were 28/0/0 and 20/0/0, and check 007 remained clean.
- **qusqybn:** No `mo check` diagnostics (001). Next: test 002, 22/0/0.
- **q5ystex:** Check 001: `E0024` at `playlist.mo:64`, bracket indexing is unavailable and `.get` returns `Option`; `E0006` at :77, character not part of Mo. Next check (002) was clean, tests 003–004 were 19/0/0 and 12/0/0, and check 005 remained clean.
- **q34fbvf:** No `mo check` diagnostics (001, 004, 005). After check 001, test 002 was 19/0/0, then test 003 was 0/0/1. The session next ran clean checks 004–005, then test 006 was 20/0/0; the log does not state why test 003 stopped.
- **qftd6gw:** Check 001: `E0050` at `temperature.mo:37`, no visible dot-call function; `E0070` at :52, missing `(Fahrenheit, Fahrenheit)` case; `E0075` at :72, arithmetic on a type without `Number`; `E0051` at :82, rebinding a visible name. Next check (002) retained `E0075` and `E0051` at the same locations. Check 003 was clean; test 004 was 15/0/0.
- **qb8avb4:** No `mo check` diagnostics (002). Initial test 001 was 12/1/0; after the clean check, test 003 was 13/0/0.
- **qhzsabr:** No `mo check` diagnostics (001). Next: test 002, 13/0/0.
- **qxjfrmq:** Check 001: `E0075` at `col-sum.mo:83`, arithmetic on a type without `Number`. Next check (002): `E1150` at :6, native call not lowered; check 003 still reported `E1150`, now at :118. Check 004 was clean; test 005 was 27/0/3. Next check (006) was clean and test 007 was 30/0/0; the log does not state why three tests stopped in 005.
- **qndjcr4:** No `mo check` diagnostics (001, 003). Each was followed by tests: 002 was 2/0/0 and 004 was 9/0/0.
- **qpwnhgh:** Check 001: `E0023` at `restock.mo:95`, expected `in`, found comma. Next check (002) was clean, tests 003–004 were 16/0/0 and 8/0/0, and check 005 remained clean.
- **q26sq2c:** Check 001: `E0050` at `temperature.mo:37`, no visible dot-call function; `E0070` at :52, missing `(Fahrenheit, Fahrenheit)` case; `E0075` at :72, arithmetic on a type without `Number`; `E0051` at :82, rebinding a visible name. Next check (002) was clean, test 003 was 15/0/0, check 004 was clean, and test 005 was 15/0/0.
- **qymabxn:** No `mo check` diagnostics (001, 003). Each was followed by tests: 002 was 1/0/0 and 004 was 5/0/0.
- **qn2bska:** No `mo check` diagnostics (001, 003). Each was followed by tests: 002 was 14/0/0 and 004 was 19/0/0.
- **q5hsca3:** No `mo check` diagnostics (002). Initial test 001 was 12/1/0; after the clean check, test 003 was 15/0/0.
- **qtzhsm2:** Check 001: `E1103` at `timesheet.mo:72`, body rule not implemented by the staged checker. Next check (002) was clean and test 003 was 11/0/0. Checks 004 and 006 were clean, followed respectively by tests 005 (16/0/0) and 007 (18/0/0).
- **qbpbv8a:** Check 001: `E0025` at `grades.mo:28`, use `or` instead of `||`. Next check (002): `E0053` at :28, combined different types; `E0050` at :42, name not visible; `E0075` at :76, arithmetic on a type without `Number`; `E0051` at :84, rebinding a visible name. The session next tried `mo test` (003), which repeated those four diagnostics and ran no tests (0/0/0). Check 004 was clean; test 005 was 18/0/0.
- **q4x6n2j:** Check 001: `E0024` at `playlist.mo:64`, bracket indexing is unavailable and `.get` returns `Option`; `E0006` at :77, character not part of Mo. Next check (002): `E0050` at :58 and :94, no visible dot-call function. Check 003 was clean; test 004 was 19/0/0.
