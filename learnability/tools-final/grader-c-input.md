# Grader C's input

Two programs the first two graders disagreed on. For each: both graders' answers, in the order they were given. Grade each program yourself, with `grader-prompt.md`'s question, and say which grader's points hold.

## qnxknxy

Grader 1:

yes
Conversion follows rules 1–7. Title, blank lines, paragraphs and list continuations, trimmed headings, and code blocks with language mapping all match. Table cells are split on unescaped `|`, with `\|` becoming `|`.
It drops the table's second line only when that line is dashes. This is the same on any real table.
Sections and filling match the splitting rule, including block-by-block filling of an oversized section, and byte sizes count newlines.
An oversized block gives `SOURCE:LINE: a block of N bytes…`, writes nothing, and exits 1. Usage errors, an unreadable SOURCE, and a non-empty OUT exit 2, and the OUT check comes first.
The output lines `NAME SIZE CHECKSUM` (two-digit names, Adler-32 in 8 hex digits) are right. It also accepts `--limit=N` and options in any position, which is a harmless extension.

Grader 2:

no
Rule 7 requires dropping the table's second, separator line, but `table` drops it only when `separator?` accepts it.
For example, the valid separator `| ---\t| --- |` contains a tab; `separator?` rejects it, so the dashes are incorrectly emitted as a data row.

## q8dzhm5

Grader 1:

yes
Walks FOLDER skipping dot-folders, sorts by path, and prints `PATH:LINE: TARGET: REASON` lines, the count line, and exit statuses 1/0/2 as the task asks (rules 1, 5, 7).
Finds inline links, images, angle-bracket targets with titles, and definitions indented ≤3; it skips code spans (matched by length) and fences (same character, at least as long); LINE is the target's line (rule 2).
Handles schemes, `/` paths, `%20`, empty paths, and leaving the folder correctly; `#` parts are checked only on `.md` files or the same file, not on folders (rules 3–5).
Anchors use a Unicode letter/digit table, keep link text, drop code marks, and add `-1`, `-2` suffixes (rule 6).
Minor gaps: heading text is not trimmed, so a trailing space adds `-`; a definition followed by junk or a single-quoted title is not counted.

Grader 2:

no
Rule 6 requires GitHub-style heading anchors, but `headings` passes untrimmed heading text to `anchor`.
For `# Hello `, the trailing space produces `hello-` rather than `hello`, so `[x](#hello)` is incorrectly reported as having no such heading (rules 5–6).
