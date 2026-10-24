# Grader C's input

Two programs the first two graders disagreed on. For each: both graders' answers, in the order they were given. Grade each program yourself, with `grader-prompt.md`'s question, and say which grader's points hold.

## qxjfrmq

Grader 1:

yes
- `strip` trims only spaces and tabs (rule 1). `parse_number` accepts `-?digits` with an optional `.` and 1–18 digits and rejects `3.`, `.5`, `1e3`, `-`, `1.2.3`, and empty fields (rule 2).
- Blank lines are skipped and counted. A wrong field count is checked before numbers, and the leftmost bad field is named; bad rows are skipped and the exit is 1 (rules 3–4).
- Exact `BigInt` sums in 10^-18 units, with two-place output rounded half away from zero (rule 5). It prints `no header` with exit 1 for empty input (rule 6).
- Like qxdevf4, it takes the first non-blank line as the header, a reasonable reading of rule 3.

Grader 2:

no
The header loop skips blank lines, contradicting the rule that the first input line is the header.
For input `\n1\n`, the program prints `1: 0.00` rather than `: 1.00`; for a single blank header line it incorrectly reports `no header` with status 1.
Its exact arithmetic and row validation do not fix that input-handling mismatch.

## qxdevf4

Grader 1:

yes
- Trims only spaces and tabs (rule 1). Its own parser accepts exactly `-?digits(.1–18 digits)?` and rejects `3.`, `.5`, `1e3`, and the empty field (rule 2).
- Checks the field count before numbers and names the leftmost bad field. It reports `line N: ...` with physical line numbers (blank lines counted), skips the row, and exits 1 (rule 4).
- Sums exactly in fixed-point `BigInt` (10^-18 units) and rounds once at output to two places, half away from zero (rule 5). It prints `no header` with exit 1 for no input (rule 6).
- It also skips blank lines before the header, a reasonable reading of rule 3, which says such lines are skipped.

Grader 2:

no
The first-line-header rule is violated: `main` skips blank lines before choosing a header.
For input `\n1\n`, it treats `1` as the header and prints `1: 0.00`, instead of totaling `1` under the empty name from the first line (`: 1.00`).
A single blank header line also incorrectly produces `no header` and status 1, although input exists.
