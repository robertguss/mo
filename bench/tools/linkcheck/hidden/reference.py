#!/usr/bin/env python3
"""The reference link checker: the task text's rules, written plainly. Hidden from sessions.

Usage: reference.py [FOLDER], run from the folder the tool would be run from.
"""

import os
import re
import sys
import unicodedata

FENCE = re.compile(r"^\s*(`{3,}|~{3,})")
HEADING = re.compile(r"^(#{1,6}) (.*)$")
SCHEME = re.compile(r"^[A-Za-z]+:")
DEFINITION = re.compile(r"^ {0,3}\[([^\]]+)\]:[ \t]*(.*)$")


def usage(message):
    print(f"linkcheck: {message}", file=sys.stderr)
    sys.exit(2)


def markdown_files(folder):
    found = []
    for here, folders, files in os.walk(folder):
        folders[:] = [f for f in folders if not f.startswith(".")]
        for name in files:
            if name.endswith(".md"):
                found.append(os.path.relpath(os.path.join(here, name), folder).replace(os.sep, "/"))
    return sorted(found)


def outside_code(lines):
    """Yields (index, line) for each line outside fenced code blocks, and None at each fence."""
    fence = None
    for index, line in enumerate(lines):
        match = FENCE.match(line)
        if fence is None:
            if match:
                fence = match.group(1)
                yield None
            else:
                yield index, line
        elif match and match.group(1)[0] == fence[0] and len(match.group(1)) >= len(fence) \
                and line.strip() == match.group(1):
            fence = None
            yield None


def paragraphs(lines):
    """Groups the lines outside code into paragraphs, split at blank lines and fences."""
    group = []
    for item in outside_code(lines):
        if item is None or not item[1].strip():
            if group:
                yield group
            group = []
        else:
            group.append(item)
    if group:
        yield group


def mask_code_spans(text):
    """Replaces each code span, backticks included, with spaces of the same length."""
    out = list(text)
    i = 0
    while i < len(text):
        if text[i] == "`":
            j = i
            while j < len(text) and text[j] == "`":
                j += 1
            run = j - i
            k = j
            close = -1
            while k < len(text):
                if text[k] == "`":
                    m = k
                    while m < len(text) and text[m] == "`":
                        m += 1
                    if m - k == run:
                        close = k
                        break
                    k = m
                else:
                    k += 1
            if close < 0:
                i = j
                continue
            for p in range(i, close + run):
                if out[p] != "\n":
                    out[p] = " "
            i = close + run
        else:
            i += 1
    return "".join(out)


def destination(text, start):
    """Parses `(target "title")` from the `(` at start; returns (target, target_start) or None."""
    i = start + 1
    while i < len(text) and text[i] in " \t\n":
        i += 1
    target_start = i
    if i < len(text) and text[i] == "<":
        end = text.find(">", i + 1)
        if end < 0 or "\n" in text[i + 1:end]:
            return None
        target = text[i + 1:end]
        target_start = i + 1
        i = end + 1
    else:
        depth = 0
        while i < len(text) and text[i] not in " \t\n":
            if text[i] == "(":
                depth += 1
            elif text[i] == ")":
                if depth == 0:
                    break
                depth -= 1
            i += 1
        target = text[target_start:i]
    while i < len(text) and text[i] in " \t\n":
        i += 1
    if i < len(text) and text[i] == '"':
        end = text.find('"', i + 1)
        if end < 0:
            return None
        i = end + 1
        while i < len(text) and text[i] in " \t\n":
            i += 1
    if i < len(text) and text[i] == ")":
        return target, target_start
    return None


def inline_links(group):
    """Yields (line_index, target) for each inline link and image in a paragraph."""
    text = "\n".join(line for _, line in group)
    starts = []
    offset = 0
    for index, line in group:
        starts.append((offset, index))
        offset += len(line) + 1
    masked = mask_code_spans(text)

    def line_at(position):
        found = starts[0][1]
        for begin, index in starts:
            if begin <= position:
                found = index
        return found

    for i, c in enumerate(masked):
        if c != "[" or (i > 0 and masked[i - 1] == "\\"):
            continue
        depth = 0
        j = i
        close = -1
        while j < len(masked):
            if masked[j] == "\\":
                j += 2
                continue
            if masked[j] == "[":
                depth += 1
            elif masked[j] == "]":
                depth -= 1
                if depth == 0:
                    close = j
                    break
            j += 1
        if close < 0 or close + 1 >= len(masked) or masked[close + 1] != "(":
            continue
        parsed = destination(masked, close + 1)
        if parsed is None:
            continue
        target, target_start = parsed
        yield line_at(target_start), target_start, text[target_start:target_start + len(target)]


def definitions(group):
    for index, line in group:
        match = DEFINITION.match(mask_code_spans(line))
        if not match:
            continue
        rest = line[match.start(2):]
        if rest.startswith("<"):
            end = rest.find(">")
            if end < 0:
                continue
            target = rest[1:end]
        else:
            target = rest.split()[0] if rest.split() else ""
        yield index, -1, target


def links(lines):
    found = []
    for group in paragraphs(lines):
        found.extend(inline_links(group))
        found.extend(definitions(group))
    return [(index, target) for index, _, target in sorted(found, key=lambda link: link[:2])]


def anchor_text(heading):
    text = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", heading)
    text = text.replace("`", "")
    kept = []
    for c in text.lower():
        category = unicodedata.category(c)
        if category.startswith("L") or category == "Nd" or c in " -_":
            kept.append(c)
    return "".join(kept).replace(" ", "-")


def anchors(lines):
    seen = {}
    result = set()
    for item in outside_code(lines):
        if item is None:
            continue
        match = HEADING.match(item[1])
        if not match:
            continue
        base = anchor_text(match.group(2).strip())
        count = seen.get(base, 0)
        seen[base] = count + 1
        result.add(base if count == 0 else f"{base}-{count}")
    return result


def read_lines(path):
    with open(path, encoding="utf-8") as f:
        return f.read().split("\n")


def check(folder, path, target, cache):
    """Returns the reason the target is broken, or None."""
    part, hash_mark, fragment = target.partition("#")
    part = part.replace("%20", " ")
    if part == "":
        resolved = path
    else:
        base = [] if part.startswith("/") else path.split("/")[:-1]
        parts = list(base)
        for piece in part.split("/"):
            if piece in ("", "."):
                continue
            if piece == "..":
                if not parts:
                    return "leaves the folder"
                parts.pop()
            else:
                parts.append(piece)
        resolved = "/".join(parts)
        full = os.path.join(folder, resolved) if resolved else folder
        if not os.path.exists(full):
            return "no such file"
    if not hash_mark:
        return None
    full = os.path.join(folder, resolved) if resolved else folder
    if not (resolved.endswith(".md") and os.path.isfile(full)):
        return None
    if resolved not in cache:
        cache[resolved] = anchors(read_lines(full))
    return None if fragment in cache[resolved] else "no such heading"


def main():
    args = sys.argv[1:]
    if len(args) > 1:
        usage("usage: linkcheck [FOLDER]")
    folder = args[0] if args else "."
    if not os.path.isdir(folder):
        usage(f"can't read the folder {folder}")
    files = markdown_files(folder)
    broken = []
    checked = 0
    cache = {}
    for path in files:
        for index, target in links(read_lines(os.path.join(folder, path))):
            if SCHEME.match(target):
                continue
            checked += 1
            reason = check(folder, path, target, cache)
            if reason:
                broken.append(f"{path}:{index + 1}: {target}: {reason}")
    for line in broken:
        print(line)
    if broken:
        print(f"{len(broken)} broken links")
        sys.exit(1)
    print(f"ok: {checked} links in {len(files)} files")


if __name__ == "__main__":
    main()
