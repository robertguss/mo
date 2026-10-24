## pjemxbj

Grader 1:

yes
Walks FOLDER through a scoped `files.within`, skips dot-folders, collects `.md` files and sorts the full paths (rule 1). Prints lines sorted by path then line, the `N broken links`/exit 1 and `ok: N links in M files`/exit 0 endings, and exit 2 with stderr for usage or unreadable folders.
Rule 2: handles inline links and images, angle-bracket targets, titles, `[label]: target` with up to 3 spaces of indent, same-character fences at least as long as the opener, and masked code spans. LINE is the line where the target starts.
Rules 3–5: skips `letters:` schemes, resolves `/` from the root and `%20` as a space, treats an empty path as the same file, and reports `leaves the folder`, `no such file`, and `no such heading`, the last only for `.md` files and not folders.
Rule 6: removes link syntax and backticks, lowercases Unicode text, keeps only letters, digits, space, `-` and `_`, and adds `-1`/`-2` suffixes. Rule 7 counts are correct.
Minor divergences: code spans are matched across the whole file rather than per paragraph, and a reference definition with trailing junk is not taken as a link. These are only edge cases.

Grader 2:

no
Heading text is not trimmed before anchor generation: `# Hello ` gets `hello-`, so `[x](#hello)` is incorrectly broken (rule 6's GitHub anchors).
Fence recognition also imposes an unstated maximum indentation of three spaces; more-indented fences leave their contents exposed as links and headings (rules 2 and 6).

## p353hzt

Grader 1:

yes
Walks FOLDER through `within`, keeps `.md` files outside dot-folders, and sorts the paths (rule 1). Prints the lines sorted by (path, line, position), the right last line and exit 1/0, and exits 2 with stderr when the walk fails or the usage is wrong.
Rule 2: handles inline links and images (nested images too), `<...>` targets, titles, and definitions with up to 3 spaces of indent. Fences are closed only by the same character at least as long. Code spans are matched within paragraphs, and LINE is the line where the target starts.
Rules 3–5: skips schemes and counts the rest. Resolves `/` from the root, `%20`, and an empty path as the same file. Reports `leaves the folder` / `no such file` / `no such heading`, the last only for `.md` paths, so a folder named `*.md` is let through.
Rule 6: removes code marks and link destinations, then lowercases and numbers duplicates. Non-ASCII characters are filtered by a punctuation/symbol blocklist, not a letter/digit test, which is close enough for realistic headings.
Minor: one argument starting with `-` matches no case in `main`, an edge case outside the task's examples.

Grader 2:

no
Skips empty fragments entirely: `[x](#)` succeeds even when the file has no empty heading anchor, contrary to rule 5.
Reference definitions are extracted without masking multiline backtick spans, so definitions inside code are checked as links (rule 2).
The Unicode anchor filter retains nonletters such as combining acute accents, violating rule 6's allowed-character set.

## puet625

Grader 1:

yes
Converts in order: the first `# ` line becomes `<table_of_contents/>`, blank lines are dropped, paragraphs and list items are joined from trimmed lines and stop at blank or block-starting lines, other headings are trimmed, and code blocks run to the next ``` line with `mo`→`ruby` and everything else `plain text` (rules 1–6).
Tables: runs of `|` lines, the dash line dropped, `<td>` per cell, cells split at unescaped `|`, trimmed, and `\|` turned into `|` (rule 7). This reproduces the example's `215 282e4557`.
Splitting: sections start at headings. A section joins the current piece if it fits, else starts a new piece, and a section over the limit is filled block by block. A block over the limit gives `SOURCE:LINE: a block of N bytes...` and exit 1 before anything is written.
Sizes are line bytes + 1, Adler-32 is computed over UTF-8 bytes as 8-digit hex, and names run `piece-01.md`, …. Usage errors, an unreadable SOURCE, or a non-empty or non-folder OUT exit 2.
Unverified: whether `files.make_folder` succeeds when OUT already exists and is empty. The Mo semantics aren't shown, and the program's tests don't cover that case.

Grader 2:

no
A table row consisting of `|` loses its empty cell: `cells` returns no cells, producing `<tr>` and `</tr>` without the required `<td></td>` (rule 7).
Such rows are included by the task's definition of a table as a run of lines starting with `|`; this also changes piece sizes and checksums.

## pfmjtu8

Grader 1:

yes
Runs are sorted by number. A non-JSON line prints `PATH:LINE: not JSON` to stderr, the run is dropped, and the exit is 1 after the table. A usage error or an unreadable LOGS exits 2. Unknown records and fields are ignored (rules 1, 8).
Errors are keyed by (code, file, function) in a Set. The function is the last `fn `/`test ` line at or before the span line, the name stops at `(`/`[`, and `(module)` is the fallback (rule 2).
An error starts when the previous check/test run did not report it. Attempts count the later check/test runs up to the first one that no longer reports it. A fix run counts only when it falls between the last report and the clean run, and an error still open at the session's end is unfixed (rules 3–5).
The table, the rows sorted by code, the total line, the round-half-up rates with `n/a`, and `tests_passed` taken from each session's last test run all match rules 6–7 and the example.

Grader 2:

no
Function names retain whitespace before `(` or `[`, instead of taking the word after `fn` (rule 2).
Changing `fn f ()` to `fn f()` between two runs that report the same diagnostic therefore falsely records a fixed error and a newly started error (rules 3–4).

#

