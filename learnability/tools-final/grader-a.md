## q8dzhm5

yes
Walks FOLDER skipping dot-folders, sorts by path, and prints `PATH:LINE: TARGET: REASON` lines, the count line, and exit statuses 1/0/2 as the task asks (rules 1, 5, 7).
Finds inline links, images, angle-bracket targets with titles, and definitions indented ≤3; it skips code spans (matched by length) and fences (same character, at least as long); LINE is the target's line (rule 2).
Handles schemes, `/` paths, `%20`, empty paths, and leaving the folder correctly; `#` parts are checked only on `.md` files or the same file, not on folders (rules 3–5).
Anchors use a Unicode letter/digit table, keep link text, drop code marks, and add `-1`, `-2` suffixes (rule 6).
Minor gaps: heading text is not trimmed, so a trailing space adds `-`; a definition followed by junk or a single-quoted title is not counted.

## qnyekzg

yes
Takes runs in three-digit order. Errors are deduplicated by code, file, and function, with the function found from that run's `src/` copy (rules 1–2).
An error starts when it is absent from the previous check/test run. Attempts count up to the first run that no longer reports it; a `fix` between the last report and that run gives by_fix; errors still open at the end are unfixed (rules 3–5).
Runs with bad JSON are reported as `PATH:LINE: not JSON` and skipped. The table is still printed and the exit status is 1.
The table, the rates rounded half up over errors − by_fix, and tests_passed from each session's last test run all match rules 6–7, and unknown records are ignored (rule 8).
Small risk: a diagnostic whose file is missing from `src/` stops the program with status 2 instead of giving `(module)`.

## q2fcu7n

no
Rule 6: anchors keep every non-ASCII character (`c.code > 127`), not just letters and digits. A heading with an em dash, curly apostrophe, or emoji (e.g. "What’s new") gets the wrong anchor, so valid links are reported broken and broken ones pass.
Rule 5: an empty fragment in the same file (`[top](#)`) is never checked. The task's reading, and the reference, report it as `no such heading`.
Smaller deviations: footnote-style definitions `[^1]: x` are skipped, fences indented 4 or more spaces aren't recognised, and backslash escapes are dropped from the reported TARGET.
The rest is sound: file walk, sorting, reasons, counts, and usage/exit codes.

## qvjrjcu

yes
Conversion follows rules 1–7. The first `# ` line becomes the table of contents and blank lines are dropped. Paragraphs and list items are trimmed and joined, stopping at the start of another block.
Headings are trimmed. Code blocks are copied exactly with `ruby`/`plain text`, and tables drop their second line and split cells on unescaped `|`.
Sections start at headings, and the title is not a heading. A section joins the current piece only if it fits; an oversized section is split block by block.
Block sizes count UTF-8 bytes including newlines. An oversized block reports its source line, writes nothing, and exits 1.
Usage errors, an unreadable SOURCE, and a non-empty or non-folder OUT exit 2. Names are `piece-NN.md`; Adler-32 is printed as 8 lowercase hex digits.

## qs3szas

yes
Runs are ordered numerically and errors are deduplicated by code, file, and function. The function comes from that run's `src/`, falling back to `(module)` (rules 1–2).
Starts, attempts, by_fix (a fix between the last report and the clearing run), and unfixed all follow rules 3–5. Each started error gets exactly one outcome.
Bad JSON lines are reported, the run is dropped as if it never happened, and the exit status is 1 after the table.
Rows are in code order with a total line. Rates round half up over errors − by_fix, with `n/a` for 0, and tests_passed sums each session's last test totals (rules 6–7).
Unknown records and fields are ignored (rule 8). Usage errors and an unreadable LOGS exit 2.

## qnxknxy

yes
Conversion follows rules 1–7. Title, blank lines, paragraphs and list continuations, trimmed headings, and code blocks with language mapping all match. Table cells are split on unescaped `|`, with `\|` becoming `|`.
It drops the table's second line only when that line is dashes. This is the same on any real table.
Sections and filling match the splitting rule, including block-by-block filling of an oversized section, and byte sizes count newlines.
An oversized block gives `SOURCE:LINE: a block of N bytes…`, writes nothing, and exits 1. Usage errors, an unreadable SOURCE, and a non-empty OUT exit 2, and the OUT check comes first.
The output lines `NAME SIZE CHECKSUM` (two-digit names, Adler-32 in 8 hex digits) are right. It also accepts `--limit=N` and options in any position, which is a harmless extension.

## Stumbles

- **q8dzhm5**: E0093 (an imported module is unused, linkcheck.mo:3) at run 001. The next run (002, `mo check`) was clean, so it was fixed on the first retry; the final program uses `Unicode16`. No later diagnostics. The tests failed once at runs 004 and 010, and each was passing by the next test run.
- **qnyekzg**: E0093 (an imported module is unused, tally.mo:3) at run 001. Run 002 `mo check` was clean, so it was fixed on the first retry. No further diagnostics; all test runs passed.
- **q2fcu7n**: E0045 (a name clashes with a name always in scope, linkcheck.mo:7) at run 001. Run 002 `mo check` was clean, so it was fixed on the first retry. No further diagnostics; tests passed (45/45).
- **qvjrjcu**: no `mo check` diagnostics in any run; all test runs passed.
- **qs3szas**:
  - Run 001 gave E0053 (values of different types are combined, bad.mo:5). The session then ran `mo test` twice: run 002 had 2 passed and 1 failed, and run 003 hit E0053 in bad.mo:5 again with nothing run.
  - Run 004 `mo check` gave E0041 (everything not on expose is private, main.mo:5) and E0078 (a function's result doesn't match its signature, orders.mo:4).
  - Run 005 had neither of those and gave E0074 (a condition isn't a yes-or-no answer, tally.mo:153). Run 006 was clean, and run 007's `mo test` passed 27/27.
  - Run 008 gave E0053 in bad.mo:5 again, and run 009's `mo test` had 2 passed and 1 failed.
  - bad.mo, main.mo and orders.mo aren't part of the tally program, so the session was apparently checking other files too.
  - Only E0074 in tally.mo belongs to the delivered program; it was fixed on the first retry.
- **qnxknxy**: E0068 (a choice arm is a catch-all, notioncopy.mo:332) at run 001. Run 002 `mo check` was clean, so it was fixed on the first retry; the arm is now `Some(_) | None`. No further diagnostics; tests passed (30, then 35).
