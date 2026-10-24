## qs3szas

yes
Errors are deduplicated by code, file and source function, and recurring errors start new episodes (rules 2–3).
Attempts, intervening fixes and unfinished episodes are counted as required (rules 4–5).
The table, rates and last-test totals follow rules 6–7; invalid JSON skips the whole run, and unknown records are ignored (rule 8).

## qnxknxy

no
Rule 7 requires dropping the table's second, separator line, but `table` drops it only when `separator?` accepts it.
For example, the valid separator `| ---\t| --- |` contains a tab; `separator?` rejects it, so the dashes are incorrectly emitted as a data row.

## q8dzhm5

no
Rule 6 requires GitHub-style heading anchors, but `headings` passes untrimmed heading text to `anchor`.
For `# Hello `, the trailing space produces `hello-` rather than `hello`, so `[x](#hello)` is incorrectly reported as having no such heading (rules 5–6).

## qnyekzg

yes
Runs are ordered, diagnostics are deduplicated by the required identity, and error episodes count retries, fix credit and unfinished errors correctly (rules 1–5).
The sorted table and half-up percentages use the required denominator, and test totals come from each session's last valid test (rules 6–7).
Invalid JSON is reported and the entire run is skipped; unknown records and unused fields are ignored (rule 8).

## qvjrjcu

yes
The conversion preserves code contents, replaces only the first title, joins paragraphs and list continuations, and converts tables with escaped pipes as required (rules 1–7).
Splitting keeps fitting sections together and splits oversized sections only between blocks, using UTF-8 byte lengths.
Oversized blocks are rejected before writing; piece names, Adler-32 checksums and the specified exit statuses are implemented.

## q2fcu7n

no
Rule 6 allows only letters, digits, spaces, hyphens and underscores in anchors, but `anchor` retains every non-ASCII character, including emoji.
Thus `# A😀` gets `a😀` instead of `a`, incorrectly breaking `[x](#a)`.
Rule 5 also requires checking an explicitly present empty fragment: `[x](#)` must fail when there is no empty heading anchor, but `problem` accepts every empty fragment.

## Stumbles list

- **qs3szas**
  - `E0053`, check 001 (`bad.mo:5`, values of different types combined): the next run was test 002, with 2 passed, 1 failed and 0 stopped. Test 003 subsequently reported `E0053` again; the log does not establish that the earlier diagnostic was fixed by an edit.
  - `E0041`, check 004 (`main.mo:5`, access to a private module member), and `E0078`, the same check (`orders.mo:4`, result/signature mismatch): the next run was check 005, which reported only `E0074`.
  - `E0074`, check 005 (`tally.mo:153`, condition not Boolean): check 006 had no diagnostics; test 007 then had 27 passed, 0 failed and 0 stopped.
  - `E0053`, check 008 (`bad.mo:5`, values of different types combined): the next and final run was test 009, with 2 passed, 1 failed and 0 stopped.
- **qnxknxy**
  - `E0068`, check 001 (`notioncopy.mo:332`, catch-all choice arm): check 002 had no diagnostics. Check 003 was also clean, followed by test 004 with 30 passed, 0 failed and 0 stopped. The final test, 006, had 35 passed, 0 failed and 0 stopped.
- **q8dzhm5**
  - `E0093`, check 001 (`linkcheck.mo:3`, unused imported module): check 002 had no diagnostics. Check 003 was also clean, followed by test 004 with 1 passed, 1 failed and 0 stopped. Later tests improved, with the final test 014 showing 7 passed, 0 failed and 0 stopped.
- **qnyekzg**
  - `E0093`, check 001 (`tally.mo:3`, unused imported module): check 002 had no diagnostics; test 003 then had 6 passed, 0 failed and 0 stopped. All subsequent checks were clean, and final test 009 had 10 passed, 0 failed and 0 stopped.
- **qvjrjcu**
  - No `mo check` diagnostics: checks 001, 003 and 005 were clean. Their following tests had respectively 1, 1 and 9 passed, with no failed or stopped tests.
- **q2fcu7n**
  - `E0045`, check 001 (`linkcheck.mo:7`, name clashes with an always-in-scope name): check 002 had no diagnostics; test 003 then had 45 passed, 0 failed and 0 stopped. Check 004 was clean, followed by test 005 with the same totals.
