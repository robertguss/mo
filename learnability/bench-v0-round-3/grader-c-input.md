# Grader C's input

One program the first two graders disagreed on: both graders' answers, in the order they were given. Grade the program yourself, with `grader-prompt.md`'s question, and say which grader's points hold.

## qkurx3c

Grader 1:

yes
Fields are trimmed of spaces and tabs and parsed by hand as `-?DIGITS(.1–18 DIGITS)?`, so `3.`, `.5`, `1e3` and `""` are rejected; the count check comes before the leftmost bad field; lines are numbered from 1 with blank lines counted and skipped (rules 1–4).
Totals are kept exactly as BigInt units of 10⁻¹⁸ and written with two places, a half rounded away from zero; empty columns give `0.00` (rule 5).
No input gives `no header` and status 1 (rule 6). The header is taken as the first non-blank line, which applies the blank-line rule to it too; that is a defensible reading.

Grader 2:

no
Treats the first nonblank line as the header instead of the first line (rule 1). For input consisting of one empty header line, it reports `no header` and exits 1, although input exists and no bad row was skipped (rule 6); with an initial blank header and later data it silently promotes data to the header.
