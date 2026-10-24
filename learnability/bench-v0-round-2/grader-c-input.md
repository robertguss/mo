# Grader C's input

One program the first two graders disagreed on: both graders' answers, in the order they were given. Grade the program yourself, with `grader-prompt.md`'s question, and say which grader's points hold.

## qh5m592

Grader 1:

yes
Task 11-calendar: `leap_year?` is fixed to the full Gregorian rule, the same as the reference. The tests are unchanged and pass (run 003).
It also rewrote `days_between` as `365·(to−from)` plus a leap-year count using floor division. The formula is correct for every year, including year 0 and negative years, when `to` is the later year, as documented. It differs from the loop only when `from > to`, which is outside the function's contract.

Grader 2:

no
The leap-year rule is correct, but the new days_between formula is not right for every Int year as required.
For from = -9223372036854775808 and to = -9223372036854775807, leap_years_before(from) evaluates year - 1 outside Int's range instead of returning the representable 366-day interval.
