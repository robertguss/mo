## Checks relative Markdown links beneath a folder.
module Linkcheck
use Unicode16

# Delta-encoded boundaries of non-ASCII Unicode letter and decimal-digit ranges.
# The library supplies Unicode case conversion, but no character-category predicate.
unicode_letters = unicode_boundaries()

fn unicode_boundaries() : List(Int)
  deltas = """
    170 1 10 1 4 1 5 23 1 31 1 458 4 12 14 5 7 1 1 1 129 5 1 2 2 4 1 1 6 1 1 3 1 1 1 20 1 83 1
    139 8 166 1 38 2 1 6 41 71 27 4 4 45 43 21 10 4 2 1 99 1 1 15 2 7 15 2 1 16 1 1 30 29 89
    11 1 14 43 9 2 4 1 5 22 4 1 9 1 3 1 23 25 7 11 5 24 1 7 16 42 58 54 3 1 18 1 7 10 4 10 1
    16 4 8 2 2 2 22 1 7 1 1 3 4 3 1 16 1 13 2 1 3 4 12 10 1 8 6 4 2 2 22 1 7 1 2 1 2 1 2 31 4
    1 1 7 10 2 3 16 9 1 3 1 22 1 7 1 2 1 5 3 1 18 1 15 2 4 10 9 1 11 8 2 2 2 22 1 7 1 2 1 5 3
    1 30 2 1 3 4 10 1 1 17 1 1 6 3 3 1 4 3 2 1 1 1 2 3 2 3 3 3 12 22 1 21 10 21 8 1 3 1 23 1
    16 3 1 26 3 1 2 2 2 4 10 16 1 4 8 1 3 1 23 1 10 1 5 3 1 30 3 1 2 4 10 1 2 17 9 1 3 1 41 2
    1 16 1 5 3 8 3 4 10 10 6 5 18 3 24 1 9 1 1 2 7 31 10 17 48 1 2 12 7 9 10 39 2 1 1 1 5 1 24
    1 1 1 10 1 2 9 1 2 5 1 1 9 10 2 4 32 1 31 10 22 8 1 36 27 5 115 43 20 11 6 6 4 4 3 1 3 2 7
    3 4 13 12 1 1 10 6 38 1 1 5 1 2 43 1 333 1 4 2 7 1 1 1 4 2 41 1 4 2 33 1 4 2 7 1 1 1 4 2
    15 1 57 1 4 2 67 37 16 16 86 2 6 3 620 2 17 1 26 5 75 6 8 7 18 13 19 14 18 14 13 1 3 15 52
    35 1 4 1 3 10 38 10 6 89 7 5 2 34 1 1 5 70 10 31 39 40 2 5 11 44 4 26 6 10 38 23 9 53 43
    10 6 10 13 1 93 47 17 8 3 10 41 30 13 56 26 36 28 10 3 49 2 11 5 43 2 3 41 4 1 6 1 2 3 1 5
    192 64 278 2 6 2 38 2 6 2 8 1 1 1 1 1 1 1 31 2 53 1 7 1 1 3 3 1 7 3 4 2 6 4 13 5 3 1 7 116
    1 13 1 16 13 101 1 4 1 2 10 1 1 3 5 6 1 1 1 1 1 1 4 1 11 2 4 5 5 4 1 52 2 2683 229 6 4 3 2
    12 38 1 1 5 1 2 56 7 1 16 23 9 7 1 7 1 7 1 7 1 7 1 7 1 7 1 7 80 1 469 2 42 5 5 2 4 86 6 3
    1 90 1 4 5 43 1 94 17 32 48 16 512 6592 64 22157 67 46 2 269 3 28 20 47 16 31 2 70 49 9 2
    103 2 82 20 17 1 3 1 4 1 23 29 52 14 50 28 10 24 6 3 1 1 2 1 38 10 23 25 29 7 47 28 11 6 5
    1 25 1 41 23 3 1 8 4 10 6 23 3 1 3 50 1 1 3 2 2 5 2 1 1 1 24 3 2 11 7 3 12 6 2 6 2 6 9 7 1
    7 1 43 1 14 6 115 13 10 6 11172 12 23 4 49 8452 366 2 106 38 7 12 5 5 1 1 10 1 13 1 5 1 1
    1 2 1 2 1 108 33 363 18 64 2 54 40 12 116 5 1 135 19 10 7 26 6 26 11 89 3 6 2 6 2 6 2 3 35
    12 1 26 1 19 1 2 1 15 2 14 34 123 389 29 3 49 47 32 13 20 1 8 6 38 10 30 2 36 4 8 48 158 2
    10 6 36 4 36 4 40 8 52 12 11 1 15 1 7 1 2 1 11 1 15 1 7 1 2 3 52 12 311 9 22 10 8 24 6 1
    42 1 9 69 6 2 1 1 44 1 2 3 1 2 23 10 23 9 31 65 19 1 2 10 22 10 26 6 26 38 56 6 2 64 1 15
    4 1 3 1 29 42 29 3 29 35 8 1 28 27 54 10 22 10 19 13 18 110 73 55 51 13 51 13 36 12 10 6
    38 9 23 250 42 6 2 16 6 56 29 10 1 8 22 42 18 46 21 27 23 12 53 46 10 1 2 2 1 13 45 32 25
    7 10 9 36 15 10 4 1 2 1 8 35 3 1 12 48 14 4 11 11 1 1 35 18 1 25 19 2 63 7 1 1 1 4 1 15 1
    10 7 47 17 10 11 8 2 2 2 22 1 7 1 2 1 5 3 1 18 1 12 5 30 10 1 1 2 1 1 38 1 1 25 1 1 1 44
    53 18 4 5 10 5 3 30 48 20 2 1 1 8 10 166 47 41 4 36 48 20 1 11 10 38 43 13 1 7 10 6 20 28
    27 21 10 6 7 185 44 116 74 21 8 2 1 2 8 1 2 1 24 15 1 1 1 14 10 70 8 2 39 16 1 1 1 28 1 10
    40 7 1 21 1 11 46 19 1 18 73 199 33 15 10 6 9 1 37 17 1 15 10 24 30 112 7 1 2 1 38 21 1 9
    10 6 6 1 2 1 32 14 1 7 10 6 44 4 10 246 19 15 1 1 13 1 34 28 10 86 1 79 922 230 196 2636
    97 15 1072 17 6 25 3995 5 583 6841 30 18 10 1734 569 7 31 1 10 6 79 1 10 6 30 18 48 16 4
    12 10 9 21 5 19 432 45 3 10 198 64 32 25 2 25 44 75 5 1 66 13 64 2 1 1 14 2 12 7382 41 32
    97 115 8701 4 1 7 1 2 1 291 15 1 29 3 2 1 14 4 8 396 2308 107 5 13 3 9 7 10 4182 10 1798
    85 1 71 1 2 2 1 2 2 2 4 1 12 1 1 1 7 1 65 1 4 2 8 1 7 1 28 1 4 1 5 1 1 3 7 1 340 2 25 1 25
    1 31 1 25 1 31 1 25 1 31 1 25 1 31 1 25 1 8 2 50 1792 31 6 6 261 62 146 45 10 7 2 10 4 1
    321 30 18 44 4 10 470 28 4 10 214 30 2 11 197 31 1 3 1 2 1 7 2 5 9 2 224 7 1 4 1 2 1 15 1
    197 59 68 7 1 4 10 1190 4 1 27 1 2 1 1 2 1 1 10 1 4 1 1 1 1 6 1 4 1 1 1 1 1 1 3 1 2 1 1 2
    1 1 1 1 1 1 1 1 1 1 2 1 1 2 4 1 7 1 4 1 4 1 1 1 10 1 17 5 3 1 5 1 17 3380 10 1030 42720 32
    4382 2 5774 2 7473 15 622 2466 542 1506 4939 5 8490
    """
  var position = 0
  var boundaries = []
  for delta in deltas.words
    position += delta.to_int or 0
    boundaries = boundaries.append(position)
  end
  boundaries
end

fn letter_or_digit?(character: Char) : Bool
  return true if character.ascii_letter? or character.ascii_digit?
  code = character.code
  return false if code < 128
  var low = 0
  var high = unicode_letters.size
  while low < high
    middle = low + (high - low) / 2
    boundary = unicode_letters.get(middle) or 0
    if boundary <= code
      low = middle + 1
    else
      high = middle
    end
  end
  low % 2 == 1
end

struct Destination
  target: Text
  start: Int
  after: Int
end

struct Inline
  destination: Destination
  label_end: Int
end

struct LinkTarget
  target: Text
  line: Int
end

struct Document
  links: List(LinkTarget)
  anchors: Set(Text)
end

fn at(chars: List(Char), index: Int) : Char
  chars.get(index) or '\u{0}'
end

fn slice(chars: List(Char), first: Int, after: Int) : Text
  Text.from_chars(chars.drop(first).take(after - first))
end

fn run_end(chars: List(Char), first: Int, mark: Char) : Int
  var index = first
  while index < chars.size and at(chars, index) == mark
    index += 1
  end
  index
end

fn outside_fences(text: Text) : Text
  var mark = '\u{0}'
  var length = 0
  var lines = []
  for line in text.lines
    chars = line.chars
    indent = run_end(chars, 0, ' ')
    candidate = at(chars, indent)
    count = run_end(chars, indent, candidate) - indent
    fence? = indent <= 3 and (candidate == '`' or candidate == '~') and count >= 3
    if length > 0
      if fence? and
        candidate == mark and
        count >= length and
        slice(chars, indent + count, chars.size).trim.empty?
        length = 0
      end
      lines = lines.append("")
    else if fence?
      mark = candidate
      length = count
      lines = lines.append("")
    else
      lines = lines.append(line)
    end
  end
  lines.join("\n")
end

fn mask_spans(chars: List(Char)) : List(Char)
  var masked = []
  var index = 0
  while index < chars.size
    current = at(chars, index)
    if current == '\\' and index + 1 < chars.size
      masked = masked.append(current).append(at(chars, index + 1))
      index += 2
      continue
    end
    if current == '`'
      after = run_end(chars, index, '`')
      width = after - index
      var search = after
      var closing = -1
      while search < chars.size
        if at(chars, search) == '`'
          next = run_end(chars, search, '`')
          if next - search == width
            closing = next
            break
          end
          search = next
        else
          search += 1
        end
      end
      if closing >= 0
        for position in index..closing
          masked = masked.append(if at(chars, position) == '\n': '\n' else: ' ')
        end
        index = closing
        continue
      end
      masked = masked.concat(chars.drop(index).take(width))
      index = after
    else
      masked = masked.append(current)
      index += 1
    end
  end
  masked
end

fn destination(chars: List(Char), first: Int, inline?: Bool) : Option(Destination)
  var index = first
  while index < chars.size and at(chars, index).ascii_white_space?
    return None if not inline? and at(chars, index) == '\n'
    index += 1
  end
  angled? = at(chars, index) == '<'
  if angled?
    index += 1
  end
  start = index
  var depth = 0
  while index < chars.size
    current = at(chars, index)
    if current == '\\' and index + 1 < chars.size
      index += 2
      continue
    end
    if angled?
      break if current == '>'
      return None if current == '\n' or current == '<'
    else
      break if current.ascii_white_space? or (current == ')' and depth == 0)
      if current == '('
        depth += 1
      else if current == ')'
        depth -= 1
      end
    end
    index += 1
  end
  return None if depth != 0
  target = slice(chars, start, index)
  if angled?
    return None if at(chars, index) != '>'
    index += 1
  end
  separator = index
  while index < chars.size and at(chars, index).ascii_white_space?
    break if not inline? and at(chars, index) == '\n'
    index += 1
  end
  if at(chars, index) == '"' and index > separator
    index += 1
    while index < chars.size and at(chars, index) != '"'
      if at(chars, index) == '\\'
        index += 1
      end
      index += 1
    end
    return None if at(chars, index) != '"'
    index += 1
    while index < chars.size and at(chars, index).ascii_white_space?
      break if not inline? and at(chars, index) == '\n'
      index += 1
    end
  end
  if inline?
    return None if at(chars, index) != ')'
    index += 1
  else
    return None if target.empty? or (index < chars.size and at(chars, index) != '\n')
  end
  Some(Destination(target: target, start: start, after: index))
end

fn inline_link(chars: List(Char), masked: List(Char), first: Int) : Option(Inline)
  var index = first + 1
  var depth = 1
  while index < chars.size
    current = at(masked, index)
    if current == '\\'
      index += 2
      continue
    end
    if current == '['
      depth += 1
    else if current == ']'
      depth -= 1
      break if depth == 0
    end
    index += 1
  end
  return None if depth != 0 or at(chars, index + 1) != '('
  parsed = try destination(chars, index + 2, true)
  Some(Inline(destination: parsed, label_end: index))
end

fn reference_link(chars: List(Char), masked: List(Char), first: Int) : Option(Destination)
  var index = run_end(chars, first, ' ')
  return None if index - first > 3 or at(masked, index) != '['
  index += 1
  label_start = index
  while index < chars.size and at(chars, index) != '\n'
    break if at(masked, index) == ']'
    if at(chars, index) == '\\'
      index += 1
    end
    index += 1
  end
  return None if index == label_start or at(masked, index) != ']' or at(chars, index + 1) != ':'
  destination(chars, index + 2, false)
end

fn links(text: Text) : List(LinkTarget)
  chars = text.chars
  masked = mask_spans(chars)
  var numbers = []
  var line = 1
  for character in chars
    numbers = numbers.append(line)
    if character == '\n'
      line += 1
    end
  end
  var found = []
  var skip = {}
  var index = 0
  while index < chars.size
    if skip.get(index) is Some(after)
      index = after
      continue
    end
    if index == 0 or at(chars, index - 1) == '\n'
      if reference_link(chars, masked, index) is Some(parsed)
        found = found.append(
          LinkTarget(target: parsed.target, line: numbers.get(parsed.start) or line)
        )
        index = parsed.after
        continue
      end
    end
    if at(masked, index) == '\\'
      index += 2
      continue
    end
    if at(masked, index) == '['
      if inline_link(chars, masked, index) is Some(parsed)
        found = found.append(
          LinkTarget(
            target: parsed.destination.target,
            line: numbers.get(parsed.destination.start) or line
          )
        )
        skip = skip.set(parsed.label_end, parsed.destination.after)
        index += 1
        continue
      end
    end
    index += 1
  end
  found
end

fn heading_words(text: Text) : Text
  chars = text.chars
  masked = mask_spans(chars)
  var words = []
  var index = 0
  while index < chars.size
    if at(masked, index) == '['
      if inline_link(chars, masked, index) is Some(parsed)
        label = slice(chars, index + 1, parsed.label_end)
        words = words.concat(heading_words(label).chars)
        index = parsed.destination.after
        continue
      end
    end
    current = at(chars, index)
    if current != '`' and current != '[' and current != ']'
      words = words.append(current)
    end
    index += 1
  end
  Text.from_chars(words)
end

fn anchors(text: Text) : Set(Text)
  var seen = {}
  var found = Set.empty
  masked_lines = Text.from_chars(mask_spans(text.chars)).lines
  for (line, masked) in text.lines.zip(masked_lines)
    chars = line.chars
    masked_chars = masked.chars
    level = run_end(masked_chars, 0, '#')
    continue if level < 1 or level > 6 or at(masked_chars, level) != ' '
    words = Unicode16.lowercase(heading_words(slice(chars, level + 1, chars.size)))
    base = Text.from_chars(
      words.chars.filter(fn(character)
        letter_or_digit?(character) or character == ' ' or character == '-' or character == '_'
      end)
    ).replace(" ", "-")
    count = seen.get(base) or 0
    anchor = if count == 0: base else: "#{base}-#{count}"
    seen = seen.set(base, count + 1)
    found = found.add(anchor)
  end
  found
end

fn external?(target: Text) : Bool
  var letters = 0
  for character in target.chars
    return letters > 0 if character == ':'
    return false if not character.ascii_letter?
    letters += 1
  end
  false
end

fn resolve(source: Text, target: Text) : Option(Text)
  examples
    resolve("docs/a.md", "../b.md") == Some("b.md")
    resolve("docs/a.md", "../../b.md") == None
    resolve("docs/a.md", "/b%20c.md") == Some("b c.md")
    resolve("docs/a.md", "") == Some("docs/a.md")
  end

  return Some(source) if target.empty?
  source_parts = source.split("/")
  var parts = if target.starts_with?("/"): [] else: source_parts.take(source_parts.size - 1)
  for part in target.replace("%20", " ").split("/")
    if part == ".."
      return None if parts.empty?
      parts = parts.take(parts.size - 1)
    else if part != "." and not part.empty?
      parts = parts.append(part)
    end
  end
  Some(if parts.empty?: "." else: parts.join("/"))
end

fn markdown_files(files: ReadFiles, folder: Text) : Result(List(Text), FileError)
  entries = try files.entries(folder)
  var paths = []
  for entry in entries
    path = if folder == ".": entry.name else: "#{folder}/#{entry.name}"
    case entry.kind
      Folder:
        if not entry.name.starts_with?(".")
          paths = paths.concat(try markdown_files(files, path))
        end
      File | Link:
        if entry.name.ends_with?(".md")
          paths = paths.append(path)
        end
      Other: Unit
    end
  end
  Ok(paths.sort)
end

fn document(text: Text) : Document
  markdown = outside_fences(text)
  Document(links: links(markdown), anchors: anchors(markdown))
end

fn broken_reason(files: ReadFiles, documents: Map(Text, Document), source: Text, target: Text) : Result(
  Option(Text),
  FileError
)
  pieces = target.split("#")
  path_part = pieces.first or ""
  path = case resolve(source, path_part)
    Some(resolved): resolved
    None: return Ok(Some("leaves the folder"))
  end
  return Ok(Some("no such file")) if not files.exists?(path)
  return Ok(None) if pieces.size < 2 or (path != source and not path.ends_with?(".md"))
  headings = case documents.get(path)
    Some(parsed): parsed.anchors
    None:
      case files.entries(path)
        Ok(_): return Ok(None)
        Error(_): Unit
      end
      text = try files.read_text(path)
      document(text).anchors
  end
  fragment = pieces.drop(1).join("#")
  Ok(if headings.has?(fragment): None else: Some("no such heading"))
end

fn main(args: Args, console: Console, files: ReadFiles) : Int
  if args.list.size > 1
    console.error("usage: linkcheck [FOLDER]")
    return 2
  end
  folder = args.list.first or "."
  if folder.starts_with?("/") or folder.starts_with?("-") or folder.empty?
    console.error("usage: linkcheck [FOLDER]")
    return 2
  end
  scoped = files.within(folder)
  paths = case markdown_files(scoped, ".")
    Ok(found): found
    Error(problem):
      console.error("#{folder}: #{problem}")
      return 2
  end
  var documents = {}
  for path in paths
    text = case scoped.read_text(path)
      Ok(contents): contents
      Error(problem):
        console.error("#{path}: #{problem}")
        return 2
    end
    documents = documents.set(path, document(text))
  end
  var checked = 0
  var broken = []
  for path in paths
    parsed = case documents.get(path)
      Some(contents): contents
      None: stop "all discovered Markdown files were loaded"
    end
    for link in parsed.links.sort_by(fn(item) item.line end)
      continue if external?(link.target)
      checked += 1
      reason = case broken_reason(scoped, documents, path, link.target)
        Ok(problem): problem
        Error(problem):
          console.error("#{path}: #{problem}")
          return 2
      end
      if reason is Some(problem)
        broken = broken.append("#{path}:#{link.line}: #{link.target}: #{problem}")
      end
    end
  end
  if not broken.empty?
    for message in broken
      console.print(message)
    end
    console.print("#{broken.size} broken links")
    return 1
  end
  console.print("ok: #{checked} links in #{paths.size} files")
  0
end

test "only Markdown files outside hidden folders are read"
  files = Files.in_memory({"README.md": "", "docs/a.md": "", ".hidden/x.md": "", "x.txt": ""})
  console = Console.fake()
  assert main(Args.of([]), console, files.read_only) == 0
  assert console.printed == ["ok: 0 links in 2 files"]
end

test "the example reports the missing guide and skips the website"
  files = Files.in_memory({
    "README.md": "See [the guide](docs/guide.md#getting-started), [missing](docs/old.md)\nand [the site](https://example.com).",
    "docs/guide.md": "## Getting started"
  })
  console = Console.fake()
  assert main(Args.of([]), console, files.read_only) == 1
  assert console.printed == ["README.md:1: docs/old.md: no such file", "1 broken links"]
  assert console.printed_errors == []
end

test "relative paths anchors and ignored fragments count correctly"
  files = Files.in_memory({
    "project/README.md": "[guide](docs/a.md#hello-world-1) ![image](<some file.png> \"Title\")\n[x](/docs/a.md#hello-world) [folder](docs#anything) [self]() [plain](plain.txt#missing)\n[mail](mailto:a@b) [url](https://example.com)",
    "project/docs/a.md": "# Hello `world`\n## Hello [world](../plain.txt)",
    "project/some file.png": "image",
    "project/plain.txt": "plain",
    "elsewhere.md": "[not scanned](missing)"
  })
  console = Console.fake()
  assert main(Args.of(["project"]), console, files.read_only) == 0
  assert console.printed == ["ok: 7 links in 2 files"]
end

test "broken links are sorted by path and target line"
  files = Files.in_memory({
    "z.md": "[missing](gone)\n[escape](../outside.md)",
    "a.md": "# Here\n[x](#absent) [x](#here-1)\n[on next line](\n<lost file.md> \"Title\")\n   [ref]: nowhere.md \"Title\""
  })
  console = Console.fake()
  assert main(Args.of([]), console, files.read_only) == 1
  assert console.printed == [
    "a.md:2: #absent: no such heading",
    "a.md:2: #here-1: no such heading",
    "a.md:4: lost file.md: no such file",
    "a.md:5: nowhere.md: no such file",
    "z.md:1: gone: no such file",
    "z.md:2: ../outside.md: leaves the folder",
    "6 broken links"
  ]
end

test "code spans and matching fences hide links and headings"
  files = Files.in_memory({
    "a.md": "# Visible\n`[hidden](gone)` ``[hidden](gone) ` more``\n~~~~ lang\n# Hidden\n[x](gone)\n~~~\n[x](gone)\n````\n[x](gone)\n~~~~~\n[ok](#visible)\n```\n[x](gone)\n```\n[bad](#hidden)"
  })
  console = Console.fake()
  assert main(Args.of([]), console, files.read_only) == 1
  assert console.printed == ["a.md:15: #hidden: no such heading", "1 broken links"]
end

test "usage and unreadable roots write only standard error"
  files = Files.in_memory({"file.txt": "text"})
  for arguments in [["a", "b"], ["missing"], ["file.txt"], ["/absolute"], ["../outside"]]
    console = Console.fake()
    assert main(Args.of(arguments), console, files.read_only) == 2
    assert console.printed == []
    assert console.printed_errors.size == 1
  end
end

test "references angles nested images and multiline code obey Markdown boundaries"
  files = Files.in_memory({
    "a.md": "[![picture](pic.png)](b.md#there)\n[label]: <b.md#there> \"[not a link](missing)\"\n    [indented]: missing\n[good](b.md#there \"[not a link](missing)\")\n`code\n# Not a heading\n[hidden](missing)`\n[bad](#not-a-heading)\n[unfinished\n: missing",
    "b.md": "# There",
    "pic.png": "image"
  })
  console = Console.fake()
  assert main(Args.of([]), console, files.read_only) == 1
  assert console.printed == ["a.md:8: #not-a-heading: no such heading", "1 broken links"]
end

test "Unicode heading words survive while punctuation and emoji do not"
  files = Files.in_memory({
    "a.md": "# [CAFÉ](#café-été-中文-١٢_) `ÉTÉ` 中文 ١٢_🚀!\n## CAFÉ ÉTÉ 中文 ١٢_!\n### CAFÉ ÉTÉ 中文 ١٢_!\n[first](#café-été-中文-١٢_-1) [second](#café-été-中文-١٢_-2)\n[![picture](pic.png)](dir.md#not-a-heading)\n[ref]: #café-été-中文-١٢_ \"Title\"\n[space](file%20name.txt)",
    "pic.png": "image",
    "dir.md/child.txt": "text",
    "file name.txt": "text"
  })
  console = Console.fake()
  assert main(Args.of([]), console, files.read_only) == 0
  assert console.printed == ["ok: 7 links in 1 files"]
end
