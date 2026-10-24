## Checks relative Markdown links within a folder.
module Linkcheck
use Unicode16

# Unicode letter and decimal-digit intervals, encoded as gap:length from the previous end.
character_runs = [
  "48:9,7:25,6:25,47:0,10:0,4:0,5:22,1:30,1:457,4:11,14:4,7:0,1:0,129:4,1:1,2:3,1:0,6:0",
  "1:2,1:0,1:19,1:82,1:138,8:165,1:37,2:0,6:40,71:26,4:3,45:42,21:9,4:1,1:98,1:0,15:1",
  "7:14,2:0,16:0,1:29,29:88,11:0,14:42,9:1,4:0,5:21,4:0,9:0,3:0,23:24,7:10,5:23,1:6",
  "16:41,58:53,3:0,18:0,7:9,4:9,1:15,4:7,2:1,2:21,1:6,1:0,3:3,3:0,16:0,13:1,1:2,4:11",
  "10:0,8:5,4:1,2:21,1:6,1:1,1:1,1:1,31:3,1:0,7:9,2:2,16:8,1:2,1:21,1:6,1:1,1:4,3:0",
  "18:0,15:1,4:9,9:0,11:7,2:1,2:21,1:6,1:1,1:4,3:0,30:1,1:2,4:9,1:0,17:0,1:5,3:2,1:3",
  "3:1,1:0,1:1,3:1,3:2,3:11,22:0,21:9,21:7,1:2,1:22,1:15,3:0,26:2,1:1,2:1,4:9,16:0,4:7",
  "1:2,1:22,1:9,1:4,3:0,30:2,1:1,4:9,1:1,17:8,1:2,1:40,2:0,16:0,5:2,8:2,4:9,10:5,5:17",
  "3:23,1:8,1:0,2:6,31:9,17:47,1:1,12:6,9:9,39:1,1:0,1:4,1:23,1:0,1:9,1:1,9:0,2:4,1:0",
  "9:9,2:3,32:0,31:9,22:7,1:35,27:4,115:42,20:10,6:5,4:3,3:0,3:1,7:2,4:12,12:0,1:9,6:37",
  "1:0,5:0,2:42,1:332,1:3,2:6,1:0,1:3,2:40,1:3,2:32,1:3,2:6,1:0,1:3,2:14,1:56,1:3,2:66",
  "37:15,16:85,2:5,3:619,2:16,1:25,5:74,6:7,7:17,13:18,14:17,14:12,1:2,15:51,35:0,4:0",
  "3:9,38:9,6:88,7:4,2:33,1:0,5:69,10:30,39:39,2:4,11:43,4:25,6:9,38:22,9:52,43:9,6:9",
  "13:0,93:46,17:7,3:9,41:29,13:55,26:35,28:9,3:48,2:10,5:42,2:2,41:3,1:5,1:1,3:0,5:191",
  "64:277,2:5,2:37,2:5,2:7,1:0,1:0,1:0,1:30,2:52,1:6,1:0,3:2,1:6,3:3,2:5,4:12,5:2,1:6",
  "116:0,13:0,16:12,101:0,4:0,2:9,1:0,3:4,6:0,1:0,1:0,1:3,1:10,2:3,5:4,4:0,52:1",
  "2683:228,6:3,3:1,12:37,1:0,5:0,2:55,7:0,16:22,9:6,1:6,1:6,1:6,1:6,1:6,1:6,1:6,80:0",
  "469:1,42:4,5:1,4:85,6:2,1:89,1:3,5:42,1:93,17:31,48:15,512:6591,64:22156,67:45,2:268",
  "3:27,20:46,16:30,2:69,49:8,2:102,2:81,20:16,1:2,1:3,1:22,29:51,14:49,28:9,24:5,3:0",
  "1:1,1:37,10:22,25:28,7:46,28:10,6:4,1:24,1:40,23:2,1:7,4:9,6:22,3:0,3:49,1:0,3:1,2:4",
  "2:0,1:0,24:2,2:10,7:2,12:5,2:5,2:5,9:6,1:6,1:42,1:13,6:114,13:9,6:11171,12:22,4:48",
  "8452:365,2:105,38:6,12:4,5:0,1:9,1:12,1:4,1:0,1:1,1:1,1:107,33:362,18:63,2:53,40:11",
  "116:4,1:134,19:9,7:25,6:25,11:88,3:5,2:5,2:5,2:2,35:11,1:25,1:18,1:1,1:14,2:13",
  "34:122,389:28,3:48,47:31,13:19,1:7,6:37,10:29,2:35,4:7,48:157,2:9,6:35,4:35,4:39",
  "8:51,12:10,1:14,1:6,1:1,1:10,1:14,1:6,1:1,3:51,12:310,9:21,10:7,24:5,1:41,1:8,69:5",
  "2:0,1:43,1:1,3:0,2:22,10:22,9:30,65:18,1:1,10:21,10:25,6:25,38:55,6:1,64:0,15:3,1:2",
  "1:28,42:28,3:28,35:7,1:27,27:53,10:21,10:18,13:17,110:72,55:50,13:50,13:35,12:9,6:37",
  "9:22,250:41,6:1,16:5,56:28,10:0,8:21,42:17,46:20,27:22,12:52,46:9,1:1,2:0,13:44",
  "32:24,7:9,9:35,15:9,4:0,2:0,8:34,3:0,12:47,14:3,11:10,1:0,35:17,1:24,19:1,63:6,1:0",
  "1:3,1:14,1:9,7:46,17:9,11:7,2:1,2:21,1:6,1:1,1:4,3:0,18:0,12:4,30:9,1:0,2:0,1:37,1:0",
  "25:0,1:0,44:52,18:3,5:9,5:2,30:47,20:1,1:0,8:9,166:46,41:3,36:47,20:0,11:9,38:42",
  "13:0,7:9,6:19,28:26,21:9,6:6,185:43,116:73,21:7,2:0,2:7,1:1,1:23,15:0,1:0,14:9,70:7",
  "2:38,16:0,1:0,28:0,10:39,7:0,21:0,11:45,19:0,18:72,199:32,15:9,6:8,1:36,17:0,15:9",
  "24:29,112:6,1:1,1:37,21:0,9:9,6:5,1:1,1:31,14:0,7:9,6:43,4:9,246:18,15:0,1:12,1:33",
  "28:9,86:0,79:921,230:195,2636:96,15:1071,17:5,25:3994,5:582,6841:29,18:9,1734:568",
  "7:30,1:9,6:78,1:9,6:29,18:47,16:3,12:9,9:20,5:18,432:44,3:9,198:63,32:24,2:24,44:74",
  "5:0,66:12,64:1,1:0,14:1,12:7381,41:31,97:114,8701:3,1:6,1:1,1:290,15:0,29:2,2:0,14:3",
  "8:395,2308:106,5:12,3:8,7:9,4182:9,1798:84,1:70,1:1,2:0,2:1,2:3,1:11,1:0,1:6,1:64",
  "1:3,2:7,1:6,1:27,1:3,1:4,1:0,3:6,1:339,2:24,1:24,1:30,1:24,1:30,1:24,1:30,1:24,1:30",
  "1:24,1:7,2:49,1792:30,6:5,261:61,146:44,10:6,2:9,4:0,321:29,18:43,4:9,470:27,4:9",
  "214:29,2:10,197:30,1:2,1:1,1:6,2:4,9:1,224:6,1:3,1:1,1:14,1:196,59:67,7:0,4:9,1190:3",
  "1:26,1:1,1:0,2:0,1:9,1:3,1:0,1:0,6:0,4:0,1:0,1:0,1:2,1:1,1:0,2:0,1:0,1:0,1:0,1:0,1:1",
  "1:0,2:3,1:6,1:3,1:3,1:0,1:9,1:16,5:2,1:4,1:16,3380:9,1030:42719,32:4381,2:5773",
  "2:7472,15:621,2466:541,1506:4938,5:8489"
].join(",")

character_ranges = decode_ranges()

fn decode_ranges() : List((Int, Int))
  var output = []
  var previous = 0
  for run in character_runs.split(",")
    parts = run.split(":")
    gap = (parts.get(0) or "").to_int or stop "the character table contains integers"
    length = (parts.get(1) or "").to_int or stop "the character table contains integers"
    first = previous + gap
    last = first + length
    output = output.append((first, last))
    previous = last + 1
  end
  output
end

fn letter_or_digit?(ch: Char) : Bool
  var low = 0
  var high = character_ranges.size
  while low < high
    middle = low + (high - low) / 2
    (first, last) = character_ranges.get(middle) or (0, 0)
    if ch.code < first
      high = middle
    else if ch.code > last
      low = middle + 1
    else
      return true
    end
  end
  false
end

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
      else if char_at(chars, close + 1) == ':' and
        index - line_start <= 3 and
        not slice(chars, index, close).contains?("\n")
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
          output = output.append(
            MarkdownLink(target: nested.target, line: nested.line + label_line - 1)
          )
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
    if ch == '`' and span_end(chars, index) is Some(after)
      marks = run_end(chars, index) - index
      output = "#{output}#{slice(chars, index + marks, after - marks)}"
      index = after
      continue
    end
    if ch == '[' and label_end(chars, index) is Some(close)
      if char_at(chars, close + 1) == '(' and destination(chars, close + 2, true) is Some(found)
        output = "#{output}#{heading_words(slice(chars, index + 1, close))}"
        index = found.after
        continue
      end
      if char_at(chars, close + 1) == '[' and label_end(chars, close + 1) is Some(after)
        output = "#{output}#{heading_words(slice(chars, index + 1, close))}"
        index = after + 1
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
    if letter_or_digit?(ch) or ch == '-' or ch == '_'
      output = "#{output}#{ch}"
    else if ch == ' '
      output = "#{output}-"
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
    base = anchor(slice(chars, count + 1, chars.size).trim)
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

fn reason(files: ReadFiles, source: Text, target: Text, known: Map(Text, Set(Text))) : Result(
  Text,
  FileError
)
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

test "Unicode anchors, hidden files, percent spaces and non-Markdown fragments"
  files = Files.in_memory({
    "site/.visible.md": "# Café العربية 中文 ١२_!😀\n[self](#café-العربية-中文-١२_) [x](.private/a.md#hidden)\n[x](a%20b.md#one) [x](asset.bin#not-a-heading) [x](folder.md#anything)\n[x]() [x](.)",
    "site/.private/a.md": "# Hidden\n[x](not-checked)",
    "site/a b.md": "# One\n# One\n# One\n[x](#one-2)",
    "site/folder.md/file.txt": "",
    "site/asset.bin": ""
  }).read_only
  console = Console.fake()
  assert main(Args.of(["site"]), console, files) == 0
  assert console.printed == ["ok: 8 links in 2 files"]
end

test "heading code stays literal and links contribute only their displayed text"
  files = Files.in_memory({
    "a.md": "# `[literal](target)` [Shown](a.md)\n[x](#literaltarget-shown)\n# [Reference][label]\n[label]: a.md\n[x](#reference)\n````mo\n# Not a heading\n```\n~~~\n# Still not a heading\n`````\n[x](#not-a-heading)"
  }).read_only
  console = Console.fake()
  assert main(Args.of([]), console, files) == 1
  assert console.printed == ["a.md:12: #not-a-heading: no such heading", "1 broken links"]
end

test "spaces around heading text are trimmed and spaces inside it are kept"
  files = Files.in_memory({
    "a.md": "# Hello \n##   Extra spaces\n### Inside  gap  \n[x](#hello) [x](#extra-spaces) [x](#inside--gap)\n[x](#hello-) [x](#--extra-spaces) [x](#inside-gap)"
  }).read_only
  console = Console.fake()
  assert main(Args.of([]), console, files) == 1
  assert console.printed == [
    "a.md:5: #hello-: no such heading",
    "a.md:5: #--extra-spaces: no such heading",
    "a.md:5: #inside-gap: no such heading",
    "3 broken links"
  ]
end
