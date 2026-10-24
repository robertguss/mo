## Converts a Markdown document into Notion's Markdown, in pieces no longer than a limit.
module Notioncopy

## One output block: its text, each line ending in a newline, and the source line it starts on.
struct Block
  line: Int
  text: Text
  heading?: Bool
end

## What the command line asks for.
struct Options
  source: Text
  out: Text
  limit: Int
end

usage = "usage: notioncopy SOURCE OUT [--limit BYTES]"

fn blank?(line: Text) : Bool
  line.trim.empty?
end

fn fence?(line: Text) : Bool
  line.starts_with?("```")
end

fn table_row?(line: Text) : Bool
  line.starts_with?("|")
end

fn heading?(line: Text) : Bool
  examples
    heading?("## Use")
    heading?("###### Six")
    not heading?("####### Seven")
    not heading?("#tag")
    not heading?("text")
  end

  chars = line.chars
  var n = 0
  while chars.get(n) == Some('#')
    n += 1
  end
  n >= 1 and n <= 6 and chars.get(n) == Some(' ')
end

fn list_item?(line: Text) : Bool
  examples
    list_item?("- a")
    list_item?("12. twelve")
    not list_item?("12.twelve")
    not list_item?(". x")
    not list_item?("-a")
  end

  return true if line.starts_with?("- ")
  chars = line.chars
  var n = 0
  while chars.get(n) is Some(c) and c.ascii_digit?
    n += 1
  end
  n >= 1 and chars.get(n) == Some('.') and chars.get(n + 1) == Some(' ')
end

fn starts_block?(line: Text) : Bool
  heading?(line) or fence?(line) or table_row?(line) or list_item?(line)
end

fn block(line: Int, parts: List(Text), heading?: Bool) : Block
  Block(line: line, text: "#{parts.join("\n")}\n", heading?: heading?)
end

## The opening line of a code block, with its language changed for Notion.
fn opening(line: Text) : Text
  examples
    opening("```mo") == "```ruby"
    opening("``` mo ") == "```ruby"
    opening("```") == "```plain text"
    opening("```python") == "```plain text"
  end

  info = (line.drop_prefix("```") or "").words
  if info.first == Some("mo"): "```ruby" else: "```plain text"
end

## The cells of a table row: split at `|` not preceded by `\`, trimmed, with `\|` read as `|`.
fn cells(row: Text) : List(Text)
  examples
    cells("| Form | Means |") == ["Form", "Means"]
    cells("| `a \\| b` | either |") == ["`a | b`", "either"]
    cells("|a|b") == ["a", "b"]
    cells("| a\\b | |") == ["a\\b", ""]
  end

  var found = []
  var current = []
  var escaped? = false
  var ended_on_pipe? = false
  for c in row.trim.chars
    ended_on_pipe? = false
    if escaped? and c == '|'
      current = current.append('|')
      escaped? = false
    else
      if escaped?
        current = current.append('\\')
        escaped? = false
      end
      if c == '\\'
        escaped? = true
      else if c == '|'
        found = found.append(Text.from_chars(current).trim)
        current = []
        ended_on_pipe? = true
      else
        current = current.append(c)
      end
    end
  end
  if escaped?
    current = current.append('\\')
  end
  if not ended_on_pipe?
    found = found.append(Text.from_chars(current).trim)
  end
  if row.trim.starts_with?("|"): found.drop(1) else: found
end

## Whether a table line is the line of dashes under the header.
fn separator?(row: Text) : Bool
  examples
    separator?("| --- | :---: |")
    not separator?("| a | b |")
  end

  chars = row.chars
  chars.contains?('-') and chars.all?(fn(c) c == '|' or c == '-' or c == ':' or c == ' ' end)
end

fn table(line: Int, rows: List(Text)) : Block
  kept = if rows.size >= 2 and separator?(rows.get(1) or ""): rows.take(1).concat(rows.drop(2)) else: rows
  var parts = ["<table header-row=\"true\">"]
  for row in kept
    parts = parts.append("<tr>")
    for cell in cells(row)
      parts = parts.append("<td>#{cell}</td>")
    end
    parts = parts.append("</tr>")
  end
  parts = parts.append("</table>")
  block(line, parts, false)
end

## The source's blocks, in order.
fn convert(source: Text) : List(Block)
  lines = source.lines
  var blocks = []
  var titled? = false
  var i = 0
  while i < lines.size
    line = lines.get(i) or ""
    start = i + 1
    if blank?(line)
      i += 1
    else if not titled? and line.starts_with?("# ")
      blocks = blocks.append(block(start, ["<table_of_contents/>"], false))
      titled? = true
      i += 1
    else if heading?(line)
      blocks = blocks.append(block(start, [line.trim], true))
      i += 1
    else if fence?(line)
      var code = [opening(line)]
      var closed? = false
      i += 1
      while i < lines.size and not closed?
        inner = lines.get(i) or ""
        code = code.append(inner)
        closed? = fence?(inner)
        i += 1
      end
      blocks = blocks.append(block(start, code, false))
    else if table_row?(line)
      var rows = []
      while i < lines.size and table_row?(lines.get(i) or "")
        rows = rows.append(lines.get(i) or "")
        i += 1
      end
      blocks = blocks.append(table(start, rows))
    else
      var words = [line.trim]
      i += 1
      while i < lines.size and
        not blank?(lines.get(i) or "") and
        not starts_block?(lines.get(i) or "")
        words = words.append((lines.get(i) or "").trim)
        i += 1
      end
      blocks = blocks.append(block(start, [words.join(" ")], false))
    end
  end
  blocks
end

## The blocks grouped into sections, each starting at a heading, except perhaps the first.
fn sections(blocks: List(Block)) : List(List(Block))
  var done = []
  var current = []
  for each in blocks
    if each.heading? and not current.empty?
      done = done.append(current)
      current = []
    end
    current = current.append(each)
  end
  if current.empty?: done else: done.append(current)
end

## The pieces' texts, filled section by section, and block by block for a section too long alone.
fn paginate(groups: List(List(Block)), limit: Int) : List(Text)
  var pieces = []
  var current = ""
  var size = 0
  for group in groups
    text = group.map(fn(b) b.text end).join("")
    length = text.byte_count
    if size + length <= limit
      current = "#{current}#{text}"
      size += length
    else if length <= limit
      pieces = pieces.append(current)
      current = text
      size = length
    else
      for each in group
        part = each.text.byte_count
        if size > 0 and size + part > limit
          pieces = pieces.append(current)
          current = ""
          size = 0
        end
        current = "#{current}#{each.text}"
        size += part
      end
    end
  end
  if size > 0: pieces.append(current) else: pieces
end

## The Adler-32 checksum of the text's UTF-8 bytes, as eight lowercase hexadecimal digits.
fn adler32(text: Text) : Text
  examples
    adler32("Wikipedia") == "11e60398"
    adler32("") == "00000001"
  end

  bytes = text.bytes
  var a = 1
  var b = 0
  for i in 0..bytes.size
    byte = case bytes.get(i)
      Some(x): x.to_int
      None: stop "i is below the size"
    end
    a = (a + byte) % 65521
    b = (b + a) % 65521
  end
  hex8(b * 65536 + a)
end

fn hex8(n: Int) : Text
  examples
    hex8(255) == "000000ff"
    hex8(0x282e4557) == "282e4557"
  end

  digits = "0123456789abcdef".chars
  var rest = n
  var out = []
  for _ in 0..8
    out = out.prepend(digits.get(rest % 16) or '0')
    rest = rest / 16
  end
  Text.from_chars(out)
end

fn piece_name(n: Int) : Text
  examples
    piece_name(1) == "piece-01.md"
    piece_name(12) == "piece-12.md"
  end

  if n < 10: "piece-0#{n}.md" else: "piece-#{n}.md"
end

fn parse_args(list: List(Text)) : Result(Options, Text)
  examples
    parse_args(["a.md", "out"]) == Ok(Options(source: "a.md", out: "out", limit: 12000))
    parse_args(["a.md", "out", "--limit", "50"]) ==
      Ok(Options(source: "a.md", out: "out", limit: 50))
    parse_args(["a.md", "out", "--limit", "0"]).map(fn(o) o.limit end) != Ok(0)
    parse_args(["a.md"]) == Error(usage)
  end

  var positional = []
  var limit = 12000
  var i = 0
  while i < list.size
    arg = list.get(i) or ""
    if arg == "--limit"
      value = try list.get(i + 1).or_fail(usage)
      limit = try parse_limit(value)
      i += 2
    else if arg.drop_prefix("--limit=") is Some(value)
      limit = try parse_limit(value)
      i += 1
    else if arg.starts_with?("-") and arg != "-"
      return Error("notioncopy: unknown option #{arg}\n#{usage}")
    else
      positional = positional.append(arg)
      i += 1
    end
  end
  case positional
    [source, out]: Ok(Options(source: source, out: out.drop_suffix("/") or out, limit: limit))
    _: Error(usage)
  end
end

fn parse_limit(text: Text) : Result(Int, Text)
  case text.to_int
    Some(n) if n > 0: Ok(n)
    Some(_) | None: Error(
      "notioncopy: the limit must be a whole number above 0, not #{text}\n#{usage}"
    )
  end
end

fn run(options: Options, console: Console, files: Files) : Int
  source = case files.read_text(options.source)
    Ok(text): text
    Error(problem):
      console.error("notioncopy: #{problem}")
      return 2
  end
  if files.exists?(options.out)
    case files.list(options.out)
      Ok([]): Unit
      Ok(_):
        console.error("notioncopy: #{options.out}: the folder is not empty")
        return 2
      Error(problem):
        console.error("notioncopy: #{problem}")
        return 2
    end
  end
  blocks = convert(source)
  limit = options.limit
  case blocks.find(fn(b) b.text.byte_count > limit end)
    Some(long):
      console.error(
        "#{options.source}:#{long.line}: a block of #{long.text.byte_count} bytes is longer than the limit"
      )
      return 1
    None: Unit
  end
  case files.make_folder(options.out)
    Ok(_): Unit
    Error(problem):
      console.error("notioncopy: #{problem}")
      return 2
  end
  for (index, piece) in paginate(sections(blocks), limit).enumerate
    name = piece_name(index + 1)
    case files.write_text("#{options.out}/#{name}", piece)
      Ok(_): Unit
      Error(problem):
        console.error("notioncopy: #{problem}")
        return 2
    end
    console.print("#{name} #{piece.byte_count} #{adler32(piece)}")
  end
  0
end

fn main(args: Args, console: Console, files: Files) : Int
  case parse_args(args.list)
    Ok(options): run(options, console, files)
    Error(message):
      console.error(message)
      2
  end
end

reference = """
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
  | `a \\| b` | either |
  """

test "the reference document becomes one piece"
  console = Console.fake()
  files = Files.in_memory({"ref.md": reference})
  assert main(Args.of(["ref.md", "out"]), console, files) == 0
  assert console.printed == ["piece-01.md 215 282e4557"]
  assert files.read_text("out/piece-01.md") is Ok(text)
  assert text.lines.take(4) == [
    "<table_of_contents/>",
    "One line wraps here.",
    "## Use",
    "- a list item that continues"
  ]
end

test "a section that doesn't fit starts the next piece, and a long one is split by blocks"
  console = Console.fake()
  files = Files.in_memory({"ref.md": reference})
  assert main(Args.of(["ref.md", "out", "--limit", "130"]), console, files) == 0
  assert console.printed == ["piece-01.md 96 48b51fff", "piece-02.md 119 ff772559"]
  whole = Console.fake()
  assert main(Args.of(["ref.md", "out2", "--limit", "200"]), whole, files) == 0
  assert whole.printed == ["piece-01.md 42 4ea00ef0", "piece-02.md 173 c1663668"]
  assert files.read_text("out2/piece-01.md") == Ok("<table_of_contents/>\nOne line wraps here.\n")
end

test "a block longer than the limit is reported and nothing is written"
  console = Console.fake()
  files = Files.in_memory({"ref.md": reference})
  assert main(Args.of(["ref.md", "out", "--limit", "100"]), console, files) == 1
  assert console.printed_errors == ["ref.md:15: a block of 119 bytes is longer than the limit"]
  assert not files.exists?("out")
end

test "usage errors, unreadable sources and full folders exit with 2"
  console = Console.fake()
  files = Files.in_memory({"ref.md": reference, "full/x": ""})
  assert main(Args.of(["ref.md"]), console, files) == 2
  assert main(Args.of(["ref.md", "out", "--limit", "x"]), console, files) == 2
  assert main(Args.of(["missing.md", "out"]), console, files) == 2
  assert main(Args.of(["ref.md", "full"]), console, files) == 2
  assert console.printed == []
end

test "code blocks keep their insides, and paragraphs end at other blocks"
  source = "text\n# not a title\n```\n# inside\n| not a table\n```\nafter\n- item\n1. next\n  more\n"
  texts = convert(source).map(fn(b) b.text end)
  assert texts == [
    "text\n",
    "<table_of_contents/>\n",
    "```plain text\n# inside\n| not a table\n```\n",
    "after\n",
    "- item\n",
    "1. next more\n"
  ]
end
