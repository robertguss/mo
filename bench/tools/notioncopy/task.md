**notioncopy:** Converts a Markdown document into Notion's Markdown, in pieces. Usage: `notioncopy SOURCE OUT [--limit BYTES]`, where SOURCE is a Markdown file, OUT is a folder that doesn't exist yet or is empty, and BYTES defaults to 12000. It writes the pieces to OUT as `piece-01.md`, `piece-02.md` and so on, and prints one line for each, `NAME SIZE CHECKSUM`, where SIZE is the piece's length in UTF-8 bytes and CHECKSUM its Adler-32 as eight lowercase hexadecimal digits. The exit status is 0. A usage error, a SOURCE that can't be read, or an OUT that isn't empty is reported and the exit status is 2. A single block longer than the limit is reported as `SOURCE:LINE: a block of N bytes is longer than the limit`, nothing is written, and the exit status is 1.

**Converting.** The source is read as lines, and each output line is one Notion block:

1. **The title.** The first line starting with `# ` is replaced by the line `<table_of_contents/>`. (The title becomes the Notion page's name, not part of its text.)
2. **Blank lines** outside code blocks are dropped.
3. **Paragraphs.** Consecutive lines of text are one paragraph, written as one line: each line trimmed of spaces at both ends, joined with one space.
4. **List items.** A line starting with `- ` or with digits, `.` and a space starts a list item. The lines after it that continue it, those that aren't blank and don't start another block, are joined to it in the same way.
5. **Headings,** lines starting with one to six `#` and a space, other than the title, are copied, trimmed.
6. **Code blocks,** from a line starting with three backticks to the next such line, are copied exactly, except that the opening line's language changes: `mo` becomes `ruby`, and anything else, or none, becomes `plain text`. Nothing inside a code block is a heading, a table or a paragraph.
7. **Tables,** runs of lines starting with `|`, become Notion tables. The second line, of dashes, is dropped. The output is `<table header-row="true">`, then for each row `<tr>`, one line `<td>CELL</td>` per cell, and `</tr>`, then `</table>`, each on its own line. Cells are separated by `|` not preceded by `\`, are trimmed, and `\|` in a cell becomes `|`.

**Splitting.** A *section* is a heading line and the lines after it up to the next heading; the lines before the first heading are a section too. A *block* is one output line, except that a code block or table is one block however many lines it has. A piece is its lines, each ending in a newline. Fill the pieces in order: a section joins the current piece if the piece stays within the limit, and otherwise starts the next piece. A section longer than the limit by itself is split between its blocks in the same way, block by block, so its first blocks join the current piece while it stays within the limit. Joining all the pieces in order gives exactly the conversion with no limit.

## Example

With `ref.md`:

````markdown
# Guide

One line
wraps here.

## Use

- a list item
  that continues

```mo
x = 1
```

| Form | Means |
| --- | --- |
| `a \| b` | either |
````

`notioncopy ref.md out` writes one piece, `out/piece-01.md`:

````text
<table_of_contents/>
One line wraps here.
## Use
- a list item that continues
```ruby
x = 1
```
<table header-row="true">
<tr>
<td>Form</td>
<td>Means</td>
</tr>
<tr>
<td>`a | b`</td>
<td>either</td>
</tr>
</table>
````

and prints:

```text
piece-01.md 215 282e4557
```
