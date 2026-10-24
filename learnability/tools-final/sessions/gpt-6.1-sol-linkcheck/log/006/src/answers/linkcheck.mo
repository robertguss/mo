## Checks relative Markdown links within a folder.
module Linkcheck
use Unicode16

struct Destination
  target: Text
  start: Int
  after: Int
end

struct MarkdownLink
  target: Text
  line: Int
end

struct Document
  path: Text
  text: Text
end

fn char_at(chars: List(Char), index: Int) : Char
  chars.get(index) or '\n'
end

fn slice(chars: List(Char), first: Int, after: Int) : Text
  Text.from_chars(chars.drop(first).take((after - first).max(0)))
end

fn run_end(chars: List(Char), start: Int) : Int
  mark = char_at(chars, start)
  var index = start
  while index < chars.size and char_at(chars, index) == mark
    index += 1
  end
  index
end

fn without_blocks(text: Text) : Text
  var mark = ' '
  var length = 0
  var output = []
  for line in text.lines
    trimmed = line.trim_start
    chars = trimmed.chars
    first = char_at(chars, 0)
    fence? = first == '`' or first == '~'
    count = if fence?: run_end(chars, 0) else: 0
    if length > 0
      if first == mark and count >= length and slice(chars, count, chars.size).trim.empty?
        length = 0
      end
      output = output.append("")
    else if fence? and count >= 3
      mark = first
      length = count
      output = output.append("")
    else
      output = output.append(line)
    end
  end
  output.join("\n")
end

fn span_end(chars: List(Char), start: Int) : Option(Int)
  after = run_end(chars, start)
  var index = after
  while index < chars.size
    if char_at(chars, index) == '`'
      next = run_end(chars, index)
      return Some(next) if next - index == after - start
      index = next
    else
      index += 1
    end
  end
  None
end

fn label_end(chars: List(Char), start: Int) : Option(Int)
  var depth = 1
  var index = start + 1
  while index < chars.size
    ch = char_at(chars, index)
    if ch == '\\'
      index += 2
      continue
    end
    if ch == '`'
      index = span_end(chars, index) or run_end(chars, index)
      continue
    end
    if ch == '['
      depth += 1
    else if ch == ']'
      depth -= 1
      return Some(index) if depth == 0
    end
    index += 1
  end
  None
end

fn destination(chars: List(Char), from: Int, inline?: Bool) : Option(Destination)
  var index = from
  while index < chars.size and char_at(chars, index).ascii_white_space?
    return None if not inline? and char_at(chars, index) == '\n'
    index += 1
  end
  start = index
  var target = ""
  if char_at(chars, index) == '<'
    index += 1
    first = index
    while index < chars.size and char_at(chars, index) != '>'
      return None if char_at(chars, index) == '\n'
      index += 1
    end
    return None if index == chars.size
    target = slice(chars, first, index)
    index += 1
  else
    var depth = 0
    while index < chars.size
      ch = char_at(chars, index)
      break if ch.ascii_white_space?
      if ch == '\\' and index + 1 < chars.size
        index += 2
        continue
      end
      if ch == '('
        depth += 1
      else if ch == ')'
        break if depth == 0
        depth -= 1
      end
      index += 1
    end
    return None if depth != 0
    target = slice(chars, start, index)
  end
  after_target = index
  while index < chars.size and char_at(chars, index).ascii_white_space?
    break if not inline? and char_at(chars, index) == '\n'
    index += 1
  end
  if index > after_target and char_at(chars, index) == '"'
    index += 1
    var closed? = false
    while index < chars.size
      ch = char_at(chars, index)
      return None if not inline? and ch == '\n'
      index += 1
      if ch == '\\' and index < chars.size
        index += 1
      else if ch == '"'
        closed? = true
        break
      end
    end
    return None if not closed?
    while index < chars.size and char_at(chars, index).ascii_white_space?
      break if not inline? and char_at(chars, index) == '\n'
      index += 1
    end
  end
  if inline?
    return None if char_at(chars, index) != ')'
    index += 1
  else
    return None if index < chars.size and char_at(chars, index) != '\n'
    return None if target.empty?
  end
  Some(Destination(target: target, start: start, after: index))
end

fn links(text: Text) : List(MarkdownLink)
  visible = without_blocks(text)
  chars = visible.chars
  var line_numbers = []
  var line = 1
  for ch in chars
    line_numbers = line_numbers.append(line)
    if ch == '\n'
      line += 1
    end
  end
  var output = []
  var index = 0
  var line_start = 0
  while index < chars.size
    ch = char_at(chars, index)
    if ch == '\n'
      line_start = index + 1
    else if ch == '\\'
      index += 2
      continue
    else if ch == '`'
      after_span = span_end(chars, index) or run_end(chars, index)
      for position in index..after_span
        if char_at(chars, position) == '\n'
          line_start = position + 1
        end
      end
      index = after_span
      continue
    else if ch == '[' and label_end(chars, index) is Some(close)
      var parsed = None
      if char_at(chars, close + 1) == '('
        parsed = destination(chars, close + 2, true)
      else if char_at(chars, close + 1) == ':' and index - line_start <= 3
        prefix = slice(chars, line_start, index)
        if prefix.chars.all?(fn(c) c == ' ' end)
          parsed = destination(chars, close + 2, false)
        end
      end
      if parsed is Some(found)
        target_line = line_numbers.get(found.start) or line
        output = output.append(MarkdownLink(target: found.target, line: target_line))
        label_line = line_numbers.get(index) or 1
        for nested in links(slice(chars, index + 1, close))
          output = output.append(MarkdownLink(
            target: nested.target, line: nested.line + label_line - 1
          ))
        end
        for position in index..found.after
          if char_at(chars, position) == '\n'
            line_start = position + 1
          end
        end
        index = found.after
        continue
      end
    end
    index += 1
  end
  output
end

fn heading_words(text: Text) : Text
  chars = text.chars
  var output = ""
  var index = 0
  while index < chars.size
    ch = char_at(chars, index)
    if ch == '[' and label_end(chars, index) is Some(close)
      if char_at(chars, close + 1) == '(' and
          destination(chars, close + 2, true) is Some(found)
        output = "#{output}#{heading_words(slice(chars, index + 1, close))}"
        index = found.after
        continue
      end
    end
    if ch != '`' and ch != '[' and ch != ']'
      output = "#{output}#{ch}"
    end
    index += 1
  end
  output
end

fn anchor(text: Text) : Text
  lowered = Unicode16.lowercase(heading_words(text))
  var output = ""
  for ch in lowered.chars
    if ch.ascii_letter? or ch.ascii_digit? or ch == '-' or ch == '_'
      output = "#{output}#{ch}"
    else if ch == ' '
      output = "#{output}-"
    else if Unicode16.uppercase(ch.to_text) != ch.to_text or
        (ch.code >= 0x3400 and ch.code <= 0x9FFF) or
        (ch.code >= 0xAC00 and ch.code <= 0xD7A3) or
        (ch.code >= 0x3041 and ch.code <= 0x3096) or
        (ch.code >= 0x30A1 and ch.code <= 0x30FA)
      output = "#{output}#{ch}"
    end
  end
  output
end

fn headings(text: Text) : Set(Text)
  var seen = {}
  var anchors = Set.empty
  for line in without_blocks(text).lines
    chars = line.chars
    continue if char_at(chars, 0) != '#'
    count = run_end(chars, 0)
    continue if count > 6 or char_at(chars, count) != ' '
    base = anchor(slice(chars, count + 1, chars.size))
    duplicates = seen.get(base) or 0
    name = if duplicates == 0: base else: "#{base}-#{duplicates}"
    seen = seen.set(base, duplicates + 1)
    anchors = anchors.add(name)
  end
  anchors
end

fn external?(target: Text) : Bool
  var letters = 0
  for ch in target.chars
    return letters > 0 if ch == ':'
    return false if not ch.ascii_letter?
    letters += 1
  end
  false
end

fn resolve(source: Text, path: Text) : Option(Text)
  return Some(source) if path.empty?
  var parts = if path.starts_with?("/"): [] else: source.split("/").reverse.drop(1).reverse
  for part in path.replace("%20", " ").split("/")
    continue if part.empty? or part == "."
    if part == ".."
      return None if parts.empty?
      parts = parts.take(parts.size - 1)
    else
      parts = parts.append(part)
    end
  end
  Some(if parts.empty?: "." else: parts.join("/"))
end

fn documents(files: ReadFiles) : Result(List(Document), FileError)
  var folders = ["."]
  var index = 0
  var output = []
  while index < folders.size
    folder = folders.get(index) or "."
    entries = try files.entries(folder)
    for entry in entries
      path = if folder == ".": entry.name else: "#{folder}/#{entry.name}"
      case entry.kind
        Folder:
          if not entry.name.starts_with?(".")
            folders = folders.append(path)
          end
        File | Link:
          if entry.name.ends_with?(".md")
            text = try files.read_text(path)
            output = output.append(Document(path: path, text: text))
          end
        Other: Unit
      end
    end
    index += 1
  end
  Ok(output.sort_by(fn(document) document.path end))
end

fn reason(files: ReadFiles, source: Text, target: Text,
    known: Map(Text, Set(Text))) : Result(Text, FileError)
  pieces = target.split("#")
  path = pieces.first or ""
  resolved = case resolve(source, path)
    Some(value): value
    None: return Ok("leaves the folder")
  end
  return Ok("no such file") if not files.exists?(resolved)
  if pieces.size > 1 and (path.empty? or resolved.ends_with?(".md"))
    return Ok("") if files.entries(resolved) is Ok(_)
    anchors = case known.get(resolved)
      Some(value): value
      None:
        contents = try files.read_text(resolved)
        headings(contents)
    end
    fragment = pieces.drop(1).join("#")
    return Ok("no such heading") if not anchors.has?(fragment)
  end
  Ok("")
end

fn inspect(files: ReadFiles, console: Console) : Result(Int, FileError)
  docs = try documents(files)
  var known = {}
  for document in docs
    known = known.set(document.path, headings(document.text))
  end
  var checked = 0
  var broken = 0
  for document in docs
    for link in links(document.text).sort_by(fn(item) item.line end)
      continue if external?(link.target)
      checked += 1
      problem = try reason(files, document.path, link.target, known)
      if not problem.empty?
        console.print("#{document.path}:#{link.line}: #{link.target}: #{problem}")
        broken += 1
      end
    end
  end
  if broken > 0
    console.print("#{broken} broken links")
    Ok(1)
  else
    console.print("ok: #{checked} links in #{docs.size} files")
    Ok(0)
  end
end

fn main(args: Args, console: Console, files: ReadFiles) : Int
  if args.list.size > 1
    console.error("usage: linkcheck [FOLDER]")
    return 2
  end
  folder = args.list.first or "."
  if folder.empty? or folder.starts_with?("/")
    console.error("usage: linkcheck [FOLDER] (FOLDER must be relative)")
    return 2
  end
  case inspect(files.within(folder), console)
    Ok(status): status
    Error(problem):
      console.error("#{problem}")
      2
  end
end

test "the example reports only the missing guide"
  files = Files.in_memory({
    "README.md": "See [the guide](docs/guide.md#getting-started), [missing](docs/old.md)\nand [the site](https://example.com).",
    "docs/guide.md": "## Getting started"
  }).read_only
  console = Console.fake()
  assert main(Args.of([]), console, files) == 1
  assert console.printed == ["README.md:1: docs/old.md: no such file", "1 broken links"]
  assert console.printed_errors == []
end

test "valid links count images and definitions but exclude code and schemes"
  files = Files.in_memory({
    "site/README.md": "# A `Small` [Heading](doc.md)\n[x](#a-small-heading) ![picture](<a b.png> \"a title\")\n   [ref]: doc.md#repeat-1 \"title\"\n`[ignore](bad)` [web](mailto:a)\n~~~~\n[ignore](bad)\n~~~\n[still ignored](bad)\n~~~~\n[x](doc.md#repeat) [x](sub/../doc.md) [root](/doc.md) [dir](sub#anything)",
    "site/doc.md": "# Repeat\n# Repeat\n",
    "site/a b.png": "image",
    "site/sub/empty.md": "",
    "site/.hidden/bad.md": "[ignored](missing)"
  }).read_only
  console = Console.fake()
  assert main(Args.of(["site"]), console, files) == 0
  assert console.printed == ["ok: 8 links in 3 files"]
  assert console.printed_errors == []
end

test "broken links are sorted by path and target line with all three reasons"
  files = Files.in_memory({
    "z.md": "[x](missing)\n[x](#absent)",
    "a/sub.md": "[x](../../gone)\n[x](\n ../gone.md)\n[x](../z.md#absent)\n[x](../z.md#)\n[x](/../gone)"
  }).read_only
  console = Console.fake()
  assert main(Args.of([]), console, files) == 1
  assert console.printed == [
    "a/sub.md:1: ../../gone: leaves the folder",
    "a/sub.md:3: ../gone.md: no such file",
    "a/sub.md:4: ../z.md#absent: no such heading",
    "a/sub.md:5: ../z.md#: no such heading",
    "a/sub.md:6: /../gone: leaves the folder",
    "z.md:1: missing: no such file",
    "z.md:2: #absent: no such heading",
    "7 broken links"
  ]
  assert console.printed_errors == []
end

test "unreadable folders and invalid usage return two on standard error"
  files = Files.in_memory({"a.md": ""}).read_only
  for arguments in [["missing"], ["a.md"], ["a", "b"], ["/absolute"], ["../outside"]]
    console = Console.fake()
    assert main(Args.of(arguments), console, files) == 2
    assert console.printed == []
    assert console.printed_errors.size == 1
  end
end

test "code span delimiters match by length and targets retain literal backticks"
  files = Files.in_memory({
    "a.md": "`` [x](bad) ` [x](bad) ``\n`multi\n[x](bad)` [x](a`b`.txt)\n[![x](a`b`.txt)](a.md)\n\\[x](bad) [a `]` b](a.md)",
    "a`b`.txt": ""
  }).read_only
  console = Console.fake()
  assert main(Args.of([]), console, files) == 0
  assert console.printed == ["ok: 4 links in 1 files"]
end
