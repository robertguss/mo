**linkcheck:** Checks that every relative link in the Markdown files below a folder leads somewhere. Usage: `linkcheck [FOLDER]`, where FOLDER is relative to the current folder and defaults to `.`. It prints one line for each broken link, in the form `PATH:LINE: TARGET: REASON`, sorted by path and then by line, then a last line: `N broken links` when there are any, and the exit status is 1; otherwise `ok: N links in M files`, and the exit status is 0. A usage error, or a FOLDER that can't be read, is reported on standard error and the exit status is 2.

The rules:

1. **Which files.** Every file whose name ends in `.md` below FOLDER, at any depth, except inside folders whose names start with `.`. PATH is the file's path from FOLDER, with `/`.
2. **Which links.** Inline links `[text](target)` and images `![alt](target)`, where the target may be followed by a title in double quotes and may be written in angle brackets, `<like this>`; and reference definitions, a line `[label]: target`, indented by at most three spaces. Text inside fenced code blocks (lines between two fences of three or more backticks or tildes; a block is closed only by a fence of the same character, at least as long as the one that opened it) and inside backtick code spans is not Markdown, so links there are not links. LINE is the line the link's target starts on.
3. **Which targets are skipped.** A target with a scheme, such as `https:`, `http:` or `mailto:` (letters, then a `:` before any `/`), is not checked. Every other target is relative.
4. **Where a target leads.** The part before any `#` is a path from the folder of the file the link is in; a path starting with `/` is from FOLDER instead. `%20` stands for a space. An empty path means the same file.
5. **The reasons.** `no such file` when nothing is at the path, a file or a folder; `leaves the folder` when the path goes above FOLDER; `no such heading` when the target has a `#` part, the path is a Markdown file (or the same file) and none of its headings has that anchor. A `#` part on a link to any other kind of file, or to a folder, is not checked.
6. **Heading anchors**, as GitHub makes them. A heading is a line starting with one to six `#` and a space, outside code blocks. Its anchor is its text in lowercase, with every character removed except letters, digits, spaces, `-` and `_`, then each space replaced by `-`. When two headings in a file have the same anchor, the second's gets `-1`, the third's `-2`, and so on. Inline code marks and link brackets in a heading's text are removed before this, keeping the words: a link in a heading counts as its text alone.
7. **Counting.** N in the `ok` line counts every link checked, skipped ones not included; M counts the Markdown files read.

## Example

With a folder holding `README.md`:

```markdown
See [the guide](docs/guide.md#getting-started), [missing](docs/old.md)
and [the site](https://example.com).
```

and `docs/guide.md` with the heading `## Getting started`, the output is:

```text
README.md:1: docs/old.md: no such file
1 broken links
```

with exit status 1.
