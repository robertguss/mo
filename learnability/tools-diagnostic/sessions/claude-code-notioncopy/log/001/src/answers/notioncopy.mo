## Converts a Markdown document into Notion's Markdown, written in pieces no longer than a limit.
module Notioncopy

## One output block: a single line, or a whole code block or table.
struct Block
  line: Int
  lines: List(Text)
  heading?: Bool
end

## What the command line asked for.
struct Options
  source: Text
  out: Text
  limit: Int
end

## A block that is longer than the limit by itself.
struct TooLong
  line: Int
  size: Int
end

usage = "usage: notioncopy SOURCE OUT [--limit BYTES]"

fn heading_level(line: Text) : Int
  examples
    heading_level("## Use") == 2
    heading_level("# Title") == 1
    heading_level("#nope") == 0
    heading_level("####### seven") == 0
    heading_level("###") == 0
  end

  var level = 0
  for c in line.chars
    if c == '#'
      level += 1
    else if c == ' ' and level >= 1 and level <= 6
      return level
    else
      return 0
    end
  end
  0
end

fn list_start?(line: Text) : Bool
  examples
    list_start?("- item")
    list_start?("12. item")
    not list_start?("12.item")
    not list_start?(". item")
    not list_start?("-item")
  end

  return true if line.starts_with?("- ")
  var digits = 0
  var dot? = false
  for c in line.chars
    if dot?
      return c == ' ' and digits > 0
    else if c.ascii_digit?
      digits += 1
    else if c == '.'
      dot? = true
    else
      return false
    end
  end
  false
end

fn fence?(line: Text) : Bool
  line.starts_with?("```")
end

fn table_row?(line: Text) : Bool
  line.starts_with?("|")
end

fn blank?(line: Text) : Bool
  line.trim.empty?
end

fn block_start?(line: Text) : Bool
  heading_level(line) > 0 or list_start?(line) or fence?(line) or table_row?(line)
end

## The cells of a table row: split at `|` not preceded by `\`, trimmed, with `\|` read as `|`.
fn cells(row: Text) : List(Text)
  examples
    cells("| a | b |") == ["a", "b"]
    cells("| `a \\| b` | either |") == ["`a | b`", "either"]
    cells("| a | |") == ["a", ""]
    cells("| a \\ b | c") == ["a \\ b", "c"]
  end

  chars = row.chars
  var pieces = []
  var current = []
  var i = 0
  while i < chars.size
    c = chars.get(i) or ' '
    if c == '\\' and chars.get(i + 1) == Some('|')
      current = current.append('|')
      i += 2
    else if c == '|'
      pieces = pieces.append(Text.from_chars(current).trim)
      current = []
      i += 1
    else
      current = current.append(c)
      i += 1
    end
  end
  last = Text.from_chars(current).trim
  if not last.empty?
    pieces = pieces.append(last)
  end
  if row.starts_with?("|"): pieces.drop(1) else: pieces
end

fn separator_row?(row: Text) : Bool
  row.chars.all?(fn(c) c == '|' or c == '-' or c == ':' or c == ' ' or c == '\t' end)
end

fn table_lines(rows: List(Text)) : List(Text)
  var out = ["<table header-row=\"true\">"]
  for (i, row) in rows.enumerate
    continue if i == 1 and separator_row?(row)
    out = out.append("<tr>")
    for cell in cells(row)
      out = out.append("<td>#{cell}</td>")
    end
    out = out.append("</tr>")
  end
  out.append("</table>")
end

fn fence_open(line: Text) : Text
  examples
    fence_open("```mo") == "```ruby"
    fence_open("``` mo ") == "```ruby"
    fence_open("```") == "```plain text"
    fence_open("```python") == "```plain text"
  end

  language = (line.drop_prefix("```") or "").trim
  if language == "mo": "```ruby" else: "```plain text"
end

## Converts the source's lines into Notion blocks, each with the line it starts on.
fn convert(source: Text) : List(Block)
  lines = source.lines
  var blocks = []
  var titled? = false
  var i = 0
  while i < lines.size
    line = lines.get(i) or ""
    start = i + 1
    if fence?(line)
      var code = [fence_open(line)]
      i += 1
      while i < lines.size
        inner = lines.get(i) or ""
        code = code.append(inner)
        i += 1
        break if fence?(inner)
      end
      blocks = blocks.append(Block(line: start, lines: code, heading?: false))
    else if blank?(line)
      i += 1
    else if not titled? and line.starts_with?("# ")
      titled? = true
      blocks = blocks.append(Block(line: start, lines: ["<table_of_contents/>"], heading?: false))
      i += 1
    else if heading_level(line) > 0
      blocks = blocks.append(Block(line: start, lines: [line.trim], heading?: true))
      i += 1
    else if table_row?(line)
      var rows = []
      while i < lines.size and table_row?(lines.get(i) or "")
        rows = rows.append(lines.get(i) or "")
        i += 1
      end
      blocks = blocks.append(Block(line: start, lines: table_lines(rows), heading?: false))
    else
      var parts = [line.trim]
      i += 1
      while i < lines.size
        next = lines.get(i) or ""
        break if blank?(next) or block_start?(next)
        parts = parts.append(next.trim)
        i += 1
      end
      blocks = blocks.append(Block(line: start, lines: [parts.join(" ")], heading?: false))
    end
  end
  blocks
end

fn size(block: Block) : Int
  block.lines.map(fn(l) l.byte_count + 1 end).sum
end

## Groups blocks into sections, each starting at a heading.
fn sections(blocks: List(Block)) : List(List(Block))
  var out = []
  var current = []
  for block in blocks
    if block.heading? and not current.empty?
      out = out.append(current)
      current = []
    end
    current = current.append(block)
  end
  if not current.empty?
    out = out.append(current)
  end
  out
end

## Fills pieces in order, keeping whole sections together where they fit.
fn split(blocks: List(Block), limit: Int) : Result(List(List(Block)), TooLong)
  var pieces = []
  var current = []
  var used = 0
  for section in sections(blocks)
    total = section.map(size).sum
    if used + total <= limit
      current = current.concat(section)
      used += total
    else if total <= limit
      pieces = pieces.append(current)
      current = section
      used = total
    else
      for block in section
        n = size(block)
        return Error(TooLong(line: block.line, size: n)) if n > limit
        if used + n > limit
          pieces = pieces.append(current)
          current = []
          used = 0
        end
        current = current.append(block)
        used += n
      end
    end
  end
  if not current.empty?
    pieces = pieces.append(current)
  end
  Ok(pieces.filter(fn(p) not p.empty? end))
end

fn piece_text(piece: List(Block)) : Text
  piece.flat_map(fn(b) b.lines end).map(fn(l) "#{l}\n" end).join("")
end

fn adler32(text: Text) : Int
  examples
    adler32("") == 1
    adler32("Wikipedia") == 0x11E60398
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
  b * 65536 + a
end

fn hex8(n: Int) : Text
  examples
    hex8(0x282e4557) == "282e4557"
    hex8(1) == "00000001"
  end

  digits = "0123456789abcdef".chars
  var out = []
  var v = n
  for _ in 0..8
    out = out.prepend(digits.get(v % 16) or '0')
    v = v / 16
  end
  Text.from_chars(out)
end

fn piece_name(n: Int) : Text
  examples
    piece_name(1) == "piece-01.md"
    piece_name(12) == "piece-12.md"
    piece_name(123) == "piece-123.md"
  end

  if n < 10: "piece-0#{n}.md" else: "piece-#{n}.md"
end

fn parse_args(list: List(Text)) : Result(Options, Text)
  var positional = []
  var limit = 12000
  var i = 0
  while i < list.size
    arg = list.get(i) or ""
    if arg == "--limit"
      value = try list.get(i + 1).or_fail("--limit needs a number of bytes")
      limit = case value.to_int
        Some(n) if n > 0: n
        _: return Error("the limit must be a whole number above 0, not #{value}")
      end
      i += 2
    else if arg.starts_with?("-") and arg != "-"
      return Error("unknown option #{arg}")
    else
      positional = positional.append(arg)
      i += 1
    end
  end
  case positional
    [source, out]: Ok(Options(source: source, out: out, limit: limit))
    _: Error("expected SOURCE and OUT")
  end
end

fn main(args: Args, console: Console, files: Files) : Int
  options = case parse_args(args.list)
    Ok(o): o
    Error(message):
      console.error("notioncopy: #{message}")
      console.error(usage)
      return 2
  end
  source = case files.read_text(options.source)
    Ok(text): text
    Error(e):
      console.error("notioncopy: #{e}")
      return 2
  end
  out = if options.out.ends_with?("/") and options.out != "/"
    options.out.drop_suffix("/") or options.out
  else
    options.out
  end
  if files.exists?(out)
    case files.list(out)
      Ok([]): Unit
      Ok(_):
        console.error("notioncopy: #{out}: not empty")
        return 2
      Error(e):
        console.error("notioncopy: #{e}")
        return 2
    end
  end
  pieces = case split(convert(source), options.limit)
    Ok(p): p
    Error(TooLong(line: line, size: n)):
      console.error("#{options.source}:#{line}: a block of #{n} bytes is longer than the limit")
      return 1
  end
  case files.make_folder(out)
    Ok(_): Unit
    Error(e):
      console.error("notioncopy: #{e}")
      return 2
  end
  for (i, piece) in pieces.enumerate
    name = piece_name(i + 1)
    text = piece_text(piece)
    case files.write_text("#{out}/#{name}", text)
      Ok(_): Unit
      Error(e):
        console.error("notioncopy: #{e}")
        return 2
    end
    console.print("#{name} #{text.byte_count} #{hex8(adler32(text))}")
  end
  0
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
  assert text.starts_with?("<table_of_contents/>\nOne line wraps here.\n## Use\n")
end
