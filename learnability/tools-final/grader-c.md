## qnxknxy

yes
Rules 1–6 hold as Grader 1 says: the title, blank lines, joined paragraphs and list items, trimmed headings, and code blocks with the `mo`→`ruby` mapping are all right. Table cells split on an unescaped `|`, and `\|` becomes `|` (rule 7).
Grader 2's point is factually right: `table` drops the second line only if `separator?` accepts it, and `separator?` allows only `|`, `-`, `:` and spaces. So `| ---\t| --- |` would be kept as a data row.
Every ordinary dash line passes that check, so rule 7 works on the tables the task describes. I count the tab case as a narrow edge, not a failure of the task.
Splitting matches: sections fill pieces, and an oversized section fills block by block. Sizes count the newlines.
Grader 1 has one detail wrong: SOURCE is read before OUT is checked, not after. The task sets no order, so this doesn't matter. The exit statuses 2 and 1 and the over-limit message are right, and nothing is written on exit 1.
The `NAME SIZE CHECKSUM` lines are right: two-digit names and 8-hex-digit Adler-32. Accepting `--limit=N` is a harmless extra.

## q8dzhm5

no
Grader 2's point holds. `headings` passes `slice(chars, count + 1, chars.size)` to `anchor` untrimmed, and `anchor` turns every space into `-`.
So `# Hello ` gives the anchor `hello-`, and `[x](#hello)` is wrongly reported as `no such heading` (rules 5–6). The reference strips the heading text, as GitHub does. Extra spaces after the `#` also give a leading `-`.
Trailing spaces on headings are common in real files, so rule 6 fails on ordinary input. Grader 1 noticed this gap but wrongly called it minor.
Grader 1's other points hold. The file walk, the sort, the output and the exits are right (rules 1, 5, 7). So are code spans and fences, and LINE as the target's line (rule 2). Schemes, `/`, `%20`, empty paths, `leaves the folder`, and skipping `#` parts on folders and non-.md files are right too (rules 3–5).
Grader 1's further remark also holds. A definition followed by junk or by a single-quoted title is not counted, but the reference counts it (rule 2). This is a lesser gap.
