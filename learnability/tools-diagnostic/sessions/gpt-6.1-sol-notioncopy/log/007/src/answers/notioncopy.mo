## Converts Markdown into byte-limited Notion Markdown pieces.
module Notioncopy

struct Block
  text: Text
  line: Int
  heading?: Bool
end

fn heading?(line: Text) : Bool
  var hashes = 0
  for ch in line.chars
    if ch == '#'
      hashes += 1
    else
      return ch == ' ' and hashes >= 1 and hashes <= 6
    end
  end
  false
end

fn list_item?(line: Text) : Bool
  return true if line.starts_with?("- ")
  chars = line.chars
  var digits = 0
  for ch in chars
    break if not ch.ascii_digit?
    digits += 1
  end
  digits > 0 and chars.get(digits) == Some('.') and chars.get(digits + 1) == Some(' ')
end

fn starts_block?(line: Text) : Bool
  heading?(line) or list_item?(line) or line.starts_with?("```") or line.starts_with?("|")
end

fn cells(row: Text) : List(Text)
  var result = []
  var current = ""
  var previous = ' '
  for ch in row.trim_end.chars.drop(1)
    if ch == '|' and previous != '\\'
      result = result.append(current.trim.replace("\\|", "|"))
      current = ""
    else
      current = "#{current}#{ch}"
    end
    previous = ch
  end
  if previous != '|' or not current.empty?
    result = result.append(current.trim.replace("\\|", "|"))
  end
  result
end

fn convert(source: Text) : List(Block)
  lines = source.lines
  var result = []
  var i = 0
  var title_seen? = false
  while i < lines.size
    line = lines.get(i) or ""
    start = i + 1
    i += 1
    continue if line.trim.empty?
    if line.starts_with?("```")
      language = (line.drop_prefix("```") or "").trim
      var code = if language == "mo": "```ruby\n" else: "```plain text\n"
      while i < lines.size
        inner = lines.get(i) or ""
        code = "#{code}#{inner}\n"
        i += 1
        break if inner.starts_with?("```")
      end
      result = result.append(Block(text: code, line: start, heading?: false))
    else if line.starts_with?("|")
      var rows = [line]
      while i < lines.size
        next = lines.get(i) or ""
        break if not next.starts_with?("|")
        rows = rows.append(next)
        i += 1
      end
      var table = "<table header-row=\"true\">\n"
      for (index, row) in rows.enumerate
        continue if index == 1
        table = "#{table}<tr>\n"
        for cell in cells(row)
          table = "#{table}<td>#{cell}</td>\n"
        end
        table = "#{table}</tr>\n"
      end
      result = result.append(Block(text: "#{table}</table>\n", line: start, heading?: false))
    else if line.starts_with?("# ") and not title_seen?
      title_seen? = true
      result = result.append(Block(text: "<table_of_contents/>\n", line: start, heading?: false))
    else if heading?(line)
      result = result.append(Block(text: "#{line.trim}\n", line: start, heading?: true))
    else
      var paragraph = line.trim
      while i < lines.size
        next = lines.get(i) or ""
        break if next.trim.empty? or starts_block?(next)
        paragraph = "#{paragraph} #{next.trim}"
        i += 1
      end
      result = result.append(Block(text: "#{paragraph}\n", line: start, heading?: false))
    end
  end
  result
end

fn split_pieces(blocks: List(Block), limit: Int) : Result(List(Text), Block)
  var sections = []
  var section = []
  for block in blocks
    return Error(block) if block.text.byte_count > limit
    if block.heading? and not section.empty?
      sections = sections.append(section)
      section = []
    end
    section = section.append(block)
  end
  if not section.empty?
    sections = sections.append(section)
  end
  var pieces = []
  var current = ""
  for group in sections
    whole = group.map(fn(b) b.text end).join("")
    if whole.byte_count <= limit
      if whole.byte_count > limit - current.byte_count
        pieces = pieces.append(current)
        current = ""
      end
      current = "#{current}#{whole}"
    else
      for block in group
        if block.text.byte_count > limit - current.byte_count
          pieces = pieces.append(current)
          current = ""
        end
        current = "#{current}#{block.text}"
      end
    end
  end
  if not current.empty?
    pieces = pieces.append(current)
  end
  Ok(pieces)
end

fn checksum(text: Text) : Text
  bytes = text.bytes
  var a = 1
  var b = 0
  for i in 0..bytes.size
    byte = case bytes.get(i)
      Some(value): value.to_int
      None: stop "the byte index is inside the byte range"
    end
    a = (a + byte) % 65521
    b = (b + a) % 65521
  end
  var value = b * 65536 + a
  var result = ""
  digits = "0123456789abcdef".chars
  for _ in 0..8
    digit = digits.get(value % 16) or '0'
    result = "#{digit}#{result}"
    value = value / 16
  end
  result
end

fn main(args: Args, console: Console, files: Files) : Int
  arguments = args.list
  if arguments.size != 2 and arguments.size != 4
    console.error("usage: notioncopy SOURCE OUT [--limit BYTES]")
    return 2
  end
  var limit = 12000
  if arguments.size == 4
    value = arguments.get(3) or ""
    if arguments.get(2) != Some("--limit") or
      value.empty? or
      not value.chars.all?(fn(ch) ch.ascii_digit? end)
      console.error("usage: notioncopy SOURCE OUT [--limit BYTES]")
      return 2
    end
    limit = case value.to_int
      Some(n) if n > 0: n
      Some(_) | None:
        console.error("BYTES must be a whole number above 0")
        return 2
    end
  end
  source = arguments.get(0) or ""
  out = arguments.get(1) or ""
  text = case files.read_text(source)
    Ok(value): value
    Error(problem):
      console.error("#{problem}")
      return 2
  end
  if files.exists?(out)
    case files.list(out)
      Ok(names):
        if not names.empty?
          console.error("#{out}: output folder is not empty")
          return 2
        end
      Error(problem):
        console.error("#{problem}")
        return 2
    end
  end
  pieces = case split_pieces(convert(text), limit)
    Ok(value): value
    Error(block):
      console.error(
        "#{source}:#{block.line}: a block of #{block.text.byte_count} bytes is longer than the limit"
      )
      return 1
  end
  case files.make_folder(out)
    Ok(_): Unit
    Error(problem):
      console.error("#{problem}")
      return 2
  end
  for (index, piece) in pieces.enumerate
    number = index + 1
    suffix = if number < 10: "0#{number}" else: "#{number}"
    name = "piece-#{suffix}.md"
    case files.write_text("#{out}/#{name}", piece)
      Ok(_): Unit
      Error(problem):
        console.error("#{problem}")
        return 2
    end
    console.print("#{name} #{piece.byte_count} #{checksum(piece)}")
  end
  0
end

test "paragraphs, list continuations and the first title are converted"
  blocks = convert("# Title\n\n One \n two \n\n- item\n continued\n# Later\n")
  assert blocks.map(fn(b) b.text end).join("") ==
    "<table_of_contents/>\nOne two\n- item continued\n# Later\n"
end

test "the published example writes exact bytes and checksum"
  input = "# Guide\n\nOne line\nwraps here.\n\n## Use\n\n- a list item\n  that continues\n\n```mo\nx = 1\n```\n\n| Form | Means |\n| --- | --- |\n| `a \\| b` | either |\n"
  expected = "<table_of_contents/>\nOne line wraps here.\n## Use\n- a list item that continues\n```ruby\nx = 1\n```\n<table header-row=\"true\">\n<tr>\n<td>Form</td>\n<td>Means</td>\n</tr>\n<tr>\n<td>`a | b`</td>\n<td>either</td>\n</tr>\n</table>\n"
  files = Files.in_memory({"ref.md": input})
  console = Console.fake()
  assert main(Args.of(["ref.md", "out"]), console, files) == 0
  assert files.read_text("out/piece-01.md") == Ok(expected)
  assert console.printed == ["piece-01.md 215 282e4557"]
  assert console.printed_errors == []
end

test "small sections move together and an oversized section fills remaining space"
  files = Files.in_memory({"source.md": "abc\n\n## A\nx\n\n## B\ny\n"})
  console = Console.fake()
  assert main(Args.of(["source.md", "out", "--limit", "10"]), console, files) == 0
  assert files.read_text("out/piece-01.md") == Ok("abc\n")
  assert files.read_text("out/piece-02.md") == Ok("## A\nx\n")
  assert files.read_text("out/piece-03.md") == Ok("## B\ny\n")
  assert files.list("out") == Ok(["piece-01.md", "piece-02.md", "piece-03.md"])

  large_files = Files.in_memory({"source.md": "abc\n\n## A\nxy\n\nz\n\npqr\n"})
  large_console = Console.fake()
  assert main(Args.of(["source.md", "out", "--limit", "10"]), large_console, large_files) == 0
  assert large_files.read_text("out/piece-01.md") == Ok("abc\n## A\n")
  assert large_files.read_text("out/piece-02.md") == Ok("xy\nz\npqr\n")
  assert large_files.list("out") == Ok(["piece-01.md", "piece-02.md"])
end

test "code contents stay literal and tables split only on unescaped pipes"
  input = "```python\n# Not a title\n\n| Not a table |\n  keep spaces  \n``` closing  \n# Real title\n###### Six  \n####### paragraph\nwrap\n1. first\n continuation\n23. second\n| A | B |\n| --- | --- |\n| x \\| y | |\n"
  expected = "```plain text\n# Not a title\n\n| Not a table |\n  keep spaces  \n``` closing  \n<table_of_contents/>\n###### Six\n####### paragraph wrap\n1. first continuation\n23. second\n<table header-row=\"true\">\n<tr>\n<td>A</td>\n<td>B</td>\n</tr>\n<tr>\n<td>x | y</td>\n<td></td>\n</tr>\n</table>\n"
  assert convert(input).map(fn(b) b.text end).join("") == expected
end

test "oversized blocks report source lines and leave output untouched"
  for input in ["ok\n\néé\n", "ok\n\n```mo\nx\n```\n", "ok\n\n| A |\n| - |\n"]
    files = Files.in_memory({"source.md": input})
    console = Console.fake()
    assert main(Args.of(["source.md", "out", "--limit", "4"]), console, files) == 1
    assert not files.exists?("out")
    assert console.printed == []
    assert console.printed_errors.size == 1
    assert (console.printed_errors.first or "").starts_with?("source.md:3: a block of ")
  end
  files = Files.in_memory({"source.md": "ok\n\néé\n"})
  assert files.make_folder("out") == Ok(Unit)
  console = Console.fake()
  assert main(Args.of(["source.md", "out", "--limit", "4"]), console, files) == 1
  assert console.printed_errors == ["source.md:3: a block of 5 bytes is longer than the limit"]
  assert files.list("out") == Ok([])
end

test "exact byte limits include the newline and count UTF-8 bytes"
  files = Files.in_memory({"source.md": "é\n\nx\n"})
  console = Console.fake()
  assert main(Args.of(["source.md", "out", "--limit", "3"]), console, files) == 0
  assert files.read_text("out/piece-01.md") == Ok("é\n")
  assert files.read_text("out/piece-02.md") == Ok("x\n")
  assert console.printed == ["piece-01.md 3 03a40177", "piece-02.md 2 00fc0083"]
end

test "invalid arguments, unreadable sources and nonempty output return usage errors"
  for arguments in [
    [],
    ["source.md"],
    ["source.md", "out", "--limit"],
    ["source.md", "out", "--other", "1"],
    ["source.md", "out", "--limit", "0"],
    ["source.md", "out", "--limit", "-1"],
    ["source.md", "out", "--limit", "1.5"],
    ["source.md", "out", "--limit", ""],
    ["missing.md", "out"]
  ]
    files = Files.in_memory({"source.md": "ok\n"})
    console = Console.fake()
    assert main(Args.of(arguments), console, files) == 2
    assert not console.printed_errors.empty?
    assert console.printed == []
    assert not files.exists?("out")
  end
  files = Files.in_memory({"source.md": "ok\n", "out/keep.md": "untouched"})
  console = Console.fake()
  assert main(Args.of(["source.md", "out"]), console, files) == 2
  assert files.read_text("out/keep.md") == Ok("untouched")
  assert files.list("out") == Ok(["keep.md"])
  assert console.printed == []
  assert not console.printed_errors.empty?
end

test "empty documents produce no pieces and an existing empty folder is accepted"
  files = Files.in_memory({"source.md": "\n  \n"})
  console = Console.fake()
  assert main(Args.of(["source.md", "out"]), console, files) == 0
  assert files.list("out") == Ok([])
  assert console.printed == []
  assert files.write_text("source.md", "a\n") == Ok(Unit)
  assert main(Args.of(["source.md", "out"]), console, files) == 0
  assert files.read_text("out/piece-01.md") == Ok("a\n")
end
