## Checks that every relative link in the Markdown files below a folder leads somewhere.
## Usage: linkcheck [FOLDER]
module Linkcheck
use Unicode16

## A link target found in a file, at a character position of the file's text.
struct Found
  target: Text
  pos: Int
end

## A link target, with the line it starts on and its position, for ordering.
struct Ref
  target: Text
  line: Int
  pos: Int
end

## A link's destination inside parentheses: its target, where the target starts,
## and the position just after the closing parenthesis.
struct Dest
  target: Text
  start: Int
  after: Int
end

## What one Markdown file holds: its links, and its headings' anchors.
struct Parsed
  refs: List(Ref)
  anchors: Set(Text)
end

## A link that leads nowhere.
struct Broken
  path: Text
  line: Int
  pos: Int
  target: Text
  reason: Text
end

fn at(cs: List(Char), i: Int) : Char
  cs.get(i) or '\u{0}'
end

fn slice(cs: List(Char), from: Int, to: Int) : Text
  Text.from_chars(cs.drop(from).take((to - from).max(0)))
end

fn run_length(cs: List(Char), from: Int, c: Char) : Int
  var k = from
  while k < cs.size and at(cs, k) == c
    k += 1
  end
  k - from
end

## Whether the line after the line break at `i` is blank, which ends a paragraph.
fn blank_line_at?(cs: List(Char), i: Int) : Bool
  var k = i + 1
  while at(cs, k) == ' ' or at(cs, k) == '\t'
    k += 1
  end
  k >= cs.size or at(cs, k) == '\n'
end

## The position after the run of `n` backticks that closes a code span, if any.
fn code_span_end(cs: List(Char), from: Int, to: Int, n: Int) : Option(Int)
  var k = from
  while k < to
    c = at(cs, k)
    if c == '`'
      m = run_length(cs, k, '`')
      return Some(k + m) if m == n
      k += m
    else if c == '\n' and blank_line_at?(cs, k)
      return None
    else
      k += 1
    end
  end
  None
end

## The position of the `]` that closes the `[` at `i`.
fn closing_bracket(cs: List(Char), i: Int, to: Int) : Option(Int)
  var depth = 0
  var k = i
  while k < to
    c = at(cs, k)
    if c == '\\'
      k += 2
    else if c == '`'
      n = run_length(cs, k, '`')
      k = code_span_end(cs, k + n, to, n) or k + n
    else if c == '\n' and blank_line_at?(cs, k)
      return None
    else
      if c == '['
        depth += 1
      else if c == ']'
        depth -= 1
        return Some(k) if depth == 0
      end
      k += 1
    end
  end
  None
end

## Skips spaces and tabs, and at most one line break.
fn skip_space(cs: List(Char), from: Int, to: Int) : Int
  var k = from
  var breaks = 0
  while k < to
    c = at(cs, k)
    if c == ' ' or c == '\t'
      k += 1
    else if c == '\n' and breaks == 0
      breaks += 1
      k += 1
    else
      break
    end
  end
  k
end

## The destination after the `(` at `open`: a target, maybe in angle brackets,
## then maybe a title, then `)`.
fn destination(cs: List(Char), open: Int, to: Int) : Option(Dest)
  start = skip_space(cs, open + 1, to)
  var k = start
  var target = ""
  var target_start = start
  if at(cs, k) == '<'
    k += 1
    while k < to and at(cs, k) != '>' and at(cs, k) != '\n' and at(cs, k) != '<'
      k += if at(cs, k) == '\\': 2 else: 1
    end
    return None if k >= to or at(cs, k) != '>'
    target = slice(cs, start + 1, k)
    target_start = start + 1
    k += 1
  else
    var depth = 0
    while k < to
      c = at(cs, k)
      if c == '\\'
        k += 2
      else if c == '('
        depth += 1
        k += 1
      else if c == ')'
        break if depth == 0
        depth -= 1
        k += 1
      else if c.ascii_white_space? or c.code < 32
        break
      else
        k += 1
      end
    end
    return None if k > to
    target = slice(cs, start, k)
  end
  var end_pos = skip_space(cs, k, to)
  quote = at(cs, end_pos)
  if end_pos > k and (quote == '"' or quote == '\'' or quote == '(')
    close = if quote == '(': ')' else: quote
    var j = end_pos + 1
    while j < to and at(cs, j) != close
      j += if at(cs, j) == '\\': 2 else: 1
    end
    return None if j >= to
    end_pos = skip_space(cs, j + 1, to)
  end
  return None if end_pos >= to or at(cs, end_pos) != ')'
  Some(Dest(target: target, start: target_start, after: end_pos + 1))
end

## The inline link or image whose text starts with the `[` at `i`: the position of
## its `]`, and its destination.
fn inline_link(cs: List(Char), i: Int, to: Int) : Option((Int, Dest))
  close = try closing_bracket(cs, i, to)
  return None if at(cs, close + 1) != '('
  dest = try destination(cs, close + 1, to)
  Some((close, dest))
end

## The targets of the inline links and images between `from` and `to`, outside code spans.
fn scan(cs: List(Char), from: Int, to: Int) : List(Found)
  var found = []
  var i = from
  while i < to
    c = at(cs, i)
    if c == '\\'
      i += 2
    else if c == '`'
      n = run_length(cs, i, '`')
      i = code_span_end(cs, i + n, to, n) or i + n
    else if c == '[' and inline_link(cs, i, to) is Some((close, dest))
      found = found.append(Found(target: dest.target, pos: dest.start)).concat(
        scan(cs, i + 1, close)
      )
      i = dest.after
    else
      i += 1
    end
  end
  found
end

## The character and length of the fence that `line` is, if it is one.
fn fence(line: Text) : Option((Char, Int))
  cs = line.trim_start.chars
  c = at(cs, 0)
  return None if c != '`' and c != '~'
  n = run_length(cs, 0, c)
  return None if n < 3
  return None if c == '`' and cs.drop(n).contains?('`')
  Some((c, n))
end

## Whether `line` closes a fenced code block opened by `n` of `c`.
fn closes?(line: Text, c: Char, n: Int) : Bool
  cs = line.trim.chars
  run = run_length(cs, 0, c)
  run >= n and run == cs.size
end

## The text of the heading `line` is, if it is one.
fn heading_text(line: Text) : Option(Text)
  cs = line.chars
  n = run_length(cs, 0, '#')
  return None if n < 1 or n > 6 or at(cs, n) != ' '
  text = slice(cs, n + 1, cs.size).trim
  rest = text.chars
  hashes = run_length(rest.reverse, 0, '#')
  return Some(text) if hashes == 0
  return Some("") if hashes == rest.size
  before = slice(rest, 0, rest.size - hashes)
  if before.ends_with?(" "): Some(before.trim_end) else: Some(text)
end

## The text of a heading without code marks and link destinations.
fn plain(cs: List(Char), from: Int, to: Int) : List(Char)
  var out = []
  var i = from
  while i < to
    c = at(cs, i)
    if c == '`'
      i += 1
    else if c == '[' and inline_link(cs, i, to) is Some((close, dest))
      out = out.concat(plain(cs, i + 1, close))
      i = dest.after
    else
      out = out.append(c)
      i += 1
    end
  end
  out
end

## Whether a character outside ASCII is punctuation or a symbol, which anchors drop.
fn symbol?(code: Int) : Bool
  latin1 = code >= 0xA0 and
    code <= 0xBF and
    not [0xAA, 0xB2, 0xB3, 0xB5, 0xB9, 0xBA, 0xBC, 0xBD, 0xBE].contains?(code)
  latin1 or
    code == 0xD7 or
    code == 0xF7 or (code >= 0x2000 and code <= 0x206F) or (code >= 0x2190 and code <= 0x2BFF) or (
      code >= 0x3000 and code <= 0x3004
    ) or (code >= 0x3008 and code <= 0x303F) or (code >= 0xFE00 and code <= 0xFE0F) or (
      code >= 0xFF01 and code <= 0xFF0F
    ) or (code >= 0x1F000 and code <= 0x1FAFF)
end

## Whether a heading's anchor keeps a character.
fn kept?(c: Char) : Bool
  if c.code < 128
    c.ascii_letter? or c.ascii_digit? or c == ' ' or c == '-' or c == '_'
  else
    not symbol?(c.code)
  end
end

## The anchor GitHub makes for a heading's text, before duplicates are numbered.
fn anchor(heading: Text) : Text
  examples
    anchor("Getting started") == "getting-started"
    anchor("Use `mo run` (fast!)") == "use-mo-run-fast"
    anchor("See [the guide](guide.md#top)") == "see-the-guide"
    anchor("A -- b_c") == "a----b_c"
  end

  cs = heading.chars
  kept = plain(cs, 0, cs.size).filter(kept?)
  Unicode16.lowercase(Text.from_chars(kept)).replace(" ", "-")
end

## The anchors of a file's headings, numbering repeats with `-1`, `-2` and so on.
fn anchors(headings: List(Text)) : Set(Text)
  var seen = {}
  var result = Set.empty
  for heading in headings
    base = anchor(heading)
    n = seen.get(base) or 0
    result = result.add(if n == 0: base else: "#{base}-#{n}")
    seen = seen.set(base, n + 1)
  end
  result
end

## The position after `[label]:` on a reference definition's line.
fn definition_colon(cs: List(Char)) : Option(Int)
  indent = run_length(cs, 0, ' ')
  return None if indent > 3 or at(cs, indent) != '['
  return None if at(cs, indent + 1) == '^' or at(cs, indent + 1) == ']'
  var j = indent + 1
  while j < cs.size and at(cs, j) != ']'
    j += if at(cs, j) == '\\': 2 else: 1
  end
  return None if j >= cs.size or at(cs, j + 1) != ':'
  Some(j + 2)
end

## The target that starts at or after `from` on a line, and its column.
fn definition_target(cs: List(Char), from: Int) : Option((Text, Int))
  var k = from
  while at(cs, k) == ' ' or at(cs, k) == '\t'
    k += 1
  end
  return None if k >= cs.size
  if at(cs, k) == '<'
    var j = k + 1
    while j < cs.size and at(cs, j) != '>'
      j += 1
    end
    return None if j >= cs.size
    return Some((slice(cs, k + 1, j), k + 1))
  end
  var j = k
  while j < cs.size and not at(cs, j).ascii_white_space?
    j += 1
  end
  Some((slice(cs, k, j), k))
end

## The line holding the position `pos`, counting from 1, given where each line starts.
fn line_at(starts: List(Int), pos: Int) : Int
  var low = 0
  var high = starts.size
  while low < high
    mid = (low + high) / 2
    if (starts.get(mid) or 0) <= pos
      low = mid + 1
    else
      high = mid
    end
  end
  low
end

## The links and heading anchors of one Markdown file's text.
fn parse(text: Text) : Parsed
  var visible = []
  var open = None
  for line in text.lines
    case open
      Some((c, n)):
        if closes?(line, c, n)
          open = None
        end
        visible = visible.append("")
      None:
        case fence(line)
          Some(f):
            open = Some(f)
            visible = visible.append("")
          None: visible = visible.append(line)
        end
    end
  end
  var starts = []
  var offset = 0
  var headings = []
  var refs = []
  for (index, line) in visible.enumerate
    starts = starts.append(offset)
    if heading_text(line) is Some(heading)
      headings = headings.append(heading)
    end
    cs = line.chars
    if definition_colon(cs) is Some(colon)
      case definition_target(cs, colon)
        Some((target, column)): refs = refs.append(
          Ref(target: target, line: index + 1, pos: offset + column)
        )
        None:
          if colon >= cs.size or cs.drop(colon).all?(fn(c) c.ascii_white_space? end)
            next = (visible.get(index + 1) or "").chars
            if definition_target(next, 0) is Some((target, column))
              next_pos = offset + cs.size + 1 + column
              refs = refs.append(Ref(target: target, line: index + 2, pos: next_pos))
            end
          end
      end
    end
    offset += cs.size + 1
  end
  all = visible.join("\n").chars
  found = scan(all, 0, all.size).map(
    fn(f) Ref(target: f.target, line: line_at(starts, f.pos), pos: f.pos) end
  )
  Parsed(refs: found.concat(refs), anchors: anchors(headings))
end

## Whether a target has a scheme, such as `https:`, and so is not checked.
fn scheme?(target: Text) : Bool
  examples
    scheme?("https://example.com")
    scheme?("mailto:ada@example.com")
    not scheme?("docs/guide.md")
    not scheme?("a/b:c")
  end

  cs = target.chars
  var n = 0
  while at(cs, n).ascii_letter?
    n += 1
  end
  n > 0 and at(cs, n) == ':'
end

## The path from the top folder that `path` leads to from the Markdown file `file`,
## or `None` when it goes above the top folder.
fn resolve(file: Text, path: Text) : Option(Text)
  examples
    resolve("README.md", "docs/guide.md") == Some("docs/guide.md")
    resolve("docs/a.md", "../b.md") == Some("b.md")
    resolve("docs/a.md", "/b/./c.md") == Some("b/c.md")
    resolve("docs/a.md", "..") == Some(".")
    resolve("docs/a.md", "../../b.md") == None
  end

  file_parts = file.split("/")
  var parts = if path.starts_with?("/"): [] else: file_parts.take(file_parts.size - 1)
  for part in path.split("/")
    if part == ".."
      return None if parts.empty?
      parts = parts.take(parts.size - 1)
    else if part != "" and part != "."
      parts = parts.append(part)
    end
  end
  Some(if parts.empty?: "." else: parts.join("/"))
end

## Whether the Markdown file at `path` has a heading with `fragment` as its anchor.
## A path that can't be read as a file, such as a folder's, is not checked.
fn has_heading?(files: ReadFiles, known: Map(Text, Set(Text)), path: Text, fragment: Text) : Bool
  case known.get(path)
    Some(found): found.has?(fragment)
    None:
      case files.read_text(path)
        Ok(text): parse(text).anchors.has?(fragment)
        Error(_): true
      end
  end
end

## Why a relative link in the file `file` leads nowhere, or `None` when it leads somewhere.
fn check(files: ReadFiles, known: Map(Text, Set(Text)), file: Text, target: Text) : Option(Text)
  pieces = target.split("#")
  raw = (pieces.first or "").replace("%20", " ")
  fragment = pieces.drop(1).join("#")
  resolved = if raw.empty?: Some(file) else: resolve(file, raw)
  case resolved
    None: Some("leaves the folder")
    Some(path):
      heading? = pieces.size > 1 and not fragment.empty? and path.ends_with?(".md")
      if not files.exists?(path)
        Some("no such file")
      else if heading? and not has_heading?(files, known, path, fragment)
        Some("no such heading")
      else
        None
      end
  end
end

## Whether a path from the top folder names a Markdown file to read: one ending in
## `.md` and not inside a folder whose name starts with `.`.
fn markdown?(path: Text) : Bool
  examples
    markdown?("README.md")
    markdown?("docs/.notes.md")
    not markdown?(".github/x.md")
    not markdown?("docs/a.txt")
  end

  parts = path.split("/")
  folders = parts.take(parts.size - 1)
  path.ends_with?(".md") and not folders.any?(fn(part) part.starts_with?(".") end)
end

fn describe(error: FileError) : Text
  case error
    NotFound(path: _): "no such folder"
    NotAllowed(path: _): "not allowed"
    NotText(path: _): "not text"
    Failed(path: _, reason: reason): reason
  end
end

## Checks the Markdown files below `folder`, prints the broken links and a summary,
## and gives the exit status.
fn run(files: ReadFiles, folder: Text, console: Console) : Int
  root = files.within(folder)
  case root.walk(".")
    Error(error):
      console.error("linkcheck: #{folder}: #{describe(error)}")
      2
    Ok(entries):
      paths = entries.filter(fn(e) e.kind == File and markdown?(e.name) end).map(fn(e) e.name end)
        .sort
      var parsed = []
      var known = {}
      for path in paths
        case root.read_text(path)
          Ok(text):
            file = parse(text)
            parsed = parsed.append((path, file))
            known = known.set(path, file.anchors)
          Error(error):
            console.error("linkcheck: #{folder}/#{path}: #{describe(error)}")
            return 2
        end
      end
      var checked = 0
      var broken = []
      for (path, file) in parsed
        for ref in file.refs
          continue if scheme?(ref.target)
          checked += 1
          case check(root, known, path, ref.target)
            Some(reason): broken = broken.append(
              Broken(path: path, line: ref.line, pos: ref.pos, target: ref.target, reason: reason)
            )
            None:
          end
        end
      end
      for b in broken.sort_by(fn(b) (b.path, b.line, b.pos) end)
        console.print("#{b.path}:#{b.line}: #{b.target}: #{b.reason}")
      end
      if broken.empty?
        console.print("ok: #{checked} links in #{parsed.size} files")
        0
      else
        console.print("#{broken.size} broken links")
        1
      end
  end
end

fn main(args: Args, console: Console, files: ReadFiles) : Int
  case args.list
    []: run(files, ".", console)
    [folder] if not folder.starts_with?("-"): run(files, folder, console)
    [_, .._]:
      console.error("usage: linkcheck [FOLDER]")
      2
  end
end

test "the example: one missing file"
  console = Console.fake()
  files = Files.in_memory({
    "README.md": "See [the guide](docs/guide.md#getting-started), [missing](docs/old.md)\nand [the site](https://example.com).\n",
    "docs/guide.md": "# Guide\n\n## Getting started\n"
  }).read_only
  assert main(Args.of([]), console, files) == 1
  assert console.printed == ["README.md:1: docs/old.md: no such file", "1 broken links"]
end
