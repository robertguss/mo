#!/usr/bin/env python3
"""The reference Notion copy: the task text's rules, written plainly. Hidden from sessions.

Usage: reference.py SOURCE OUT [--limit BYTES], run from the folder the tool would be run from.
"""

import os
import re
import sys
import zlib

LIST_ITEM = re.compile(r"^(- |[0-9]+\. )")
HEADING = re.compile(r"^#{1,6} ")


def usage(message):
    print(f"notioncopy: {message}", file=sys.stderr)
    sys.exit(2)


def source_lines(text):
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    return [line[:-1] if line.endswith("\r") else line for line in lines]


def starts_block(line, title_seen):
    return (line.startswith("```") or line.startswith("|") or HEADING.match(line) is not None
            or LIST_ITEM.match(line) is not None or (not title_seen and line.startswith("# ")))


def cells(row):
    row = row.strip()
    pieces, current, i = [], "", 0
    while i < len(row):
        if row[i] == "\\" and i + 1 < len(row) and row[i + 1] == "|":
            current += "|"
            i += 2
        elif row[i] == "|":
            pieces.append(current)
            current = ""
            i += 1
        else:
            current += row[i]
            i += 1
    pieces.append(current)
    if row.startswith("|"):
        pieces = pieces[1:]
    if len(pieces) > 1 and row.endswith("|") and not row.endswith("\\|"):
        pieces = pieces[:-1]
    return [p.strip() for p in pieces]


def convert(lines):
    """Returns a list of (heading?, block lines, source line number) in order."""
    blocks = []
    title_seen = False
    i = 0
    while i < len(lines):
        line = lines[i]
        number = i + 1
        if line.startswith("```"):
            language = line[3:].strip()
            out = ["```ruby" if language == "mo" else "```plain text"]
            i += 1
            while i < len(lines) and not lines[i].startswith("```"):
                out.append(lines[i])
                i += 1
            if i < len(lines):
                out.append(lines[i])
                i += 1
            blocks.append((False, out, number))
        elif not line.strip():
            i += 1
        elif not title_seen and line.startswith("# "):
            title_seen = True
            blocks.append((False, ["<table_of_contents/>"], number))
            i += 1
        elif HEADING.match(line):
            blocks.append((True, [line.strip()], number))
            i += 1
        elif line.startswith("|"):
            rows = []
            while i < len(lines) and lines[i].startswith("|"):
                rows.append(lines[i])
                i += 1
            del rows[1:2]
            out = ['<table header-row="true">']
            for row in rows:
                out.append("<tr>")
                out.extend(f"<td>{cell}</td>" for cell in cells(row))
                out.append("</tr>")
            out.append("</table>")
            blocks.append((False, out, number))
        else:
            text = [line.strip()]
            i += 1
            while i < len(lines) and lines[i].strip() and not starts_block(lines[i], title_seen):
                text.append(lines[i].strip())
                i += 1
            blocks.append((False, [" ".join(text)], number))
    return blocks


def size(block):
    return sum(len(line.encode()) + 1 for line in block[1])


def split(blocks, limit):
    sections = []
    for block in blocks:
        if block[0] or not sections:
            sections.append([])
        sections[-1].append(block)
    pieces = [[]]

    def used():
        return sum(size(b) for b in pieces[-1])

    for section in sections:
        total = sum(size(b) for b in section)
        if total <= limit:
            if used() + total > limit:
                pieces.append([])
            pieces[-1].extend(section)
        else:
            for block in section:
                if used() + size(block) > limit:
                    pieces.append([])
                pieces[-1].append(block)
    return [p for p in pieces if p]


def main():
    args = sys.argv[1:]
    limit = 12000
    if len(args) == 4 and args[2] == "--limit":
        if not args[3].isdigit() or int(args[3]) == 0:
            usage("--limit needs a number of bytes above 0")
        limit = int(args[3])
        args = args[:2]
    if len(args) != 2:
        usage("usage: notioncopy SOURCE OUT [--limit BYTES]")
    source, out = args
    try:
        with open(source, encoding="utf-8") as f:
            text = f.read()
    except (OSError, UnicodeDecodeError):
        usage(f"can't read {source}")
    if os.path.exists(out) and (not os.path.isdir(out) or os.listdir(out)):
        usage(f"{out} isn't an empty folder")
    blocks = convert(source_lines(text))
    for block in blocks:
        if size(block) > limit:
            print(f"{source}:{block[2]}: a block of {size(block)} bytes is longer than the limit",
                  file=sys.stderr)
            sys.exit(1)
    os.makedirs(out, exist_ok=True)
    for number, piece in enumerate(split(blocks, limit), 1):
        data = "".join(line + "\n" for block in piece for line in block[1]).encode()
        name = f"piece-{number:02d}.md"
        with open(os.path.join(out, name), "wb") as f:
            f.write(data)
        print(f"{name} {len(data)} {zlib.adler32(data):08x}")


if __name__ == "__main__":
    main()
