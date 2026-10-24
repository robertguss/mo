## Checks relative Markdown links beneath a folder.
module Linkcheck

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
  return None if index == label_start or at(chars, index + 1) != ':'
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
  var index = 0
  while index < chars.size
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
        index = parsed.destination.after
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
  for line in text.lines
    chars = line.chars
    level = run_end(chars, 0, '#')
    continue if level < 1 or level > 6 or at(chars, level) != ' '
    words = heading_words(slice(chars, level + 1, chars.size)).ascii_lowercase
    base = Text.from_chars(
      words.chars.filter(fn(character)
        character.ascii_letter? or
          character.ascii_digit? or
          character == ' ' or
          character == '-' or
          character == '_'
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
  for path in paths
    case scoped.read_text(path)
      Ok(_): Unit
      Error(problem):
        console.error("#{path}: #{problem}")
        return 2
    end
  end
  console.print("ok: 0 links in #{paths.size} files")
  0
end

test "only Markdown files outside hidden folders are read"
  files = Files.in_memory({"README.md": "", "docs/a.md": "", ".hidden/x.md": "", "x.txt": ""})
  console = Console.fake()
  assert main(Args.of([]), console, files.read_only) == 0
  assert console.printed == ["ok: 0 links in 2 files"]
end
