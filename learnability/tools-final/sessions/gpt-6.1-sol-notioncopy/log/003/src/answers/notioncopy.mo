## Converts Markdown into byte-limited pieces for Notion.
module Notioncopy

struct Block
  text: Text
  line: Int
  heading?: Bool
end

fn heading?(line: Text) : Bool
  var hashes = 0
  for char in line.chars
    if char == '#'
      hashes += 1
    else
      return hashes >= 1 and hashes <= 6 and char == ' '
    end
  end
  false
end

fn list_item?(line: Text) : Bool
  return true if line.starts_with?("- ")
  var digits = 0
  remaining = line.chars
  for char in remaining
    if char.ascii_digit?
      digits += 1
    else
      return digits > 0 and Text.from_chars(remaining.drop(digits)).starts_with?(". ")
    end
  end
  false
end

fn starts_block?(line: Text) : Bool
  heading?(line) or list_item?(line) or line.starts_with?("```") or line.starts_with?("|")
end

fn cells(row: Text) : List(Text)
  var result = []
  var cell = ""
  var previous = ' '
  body = (row.drop_prefix("|") or row).trim_end
  for char in body.chars
    if char == '|' and previous != '\\'
      result = result.append(cell.trim.replace("\\|", "|"))
      cell = ""
    else
      cell = "#{cell}#{char}"
    end
    previous = char
  end
  if not body.ends_with?("|") or previous == '|' and body.ends_with?("\\|")
    result = result.append(cell.trim.replace("\\|", "|"))
  end
  result
end

fn table(rows: List(Text)) : Text
  var output = ["<table header-row=\"true\">"]
  for (index, row) in rows.enumerate
    continue if index == 1
    output = output.append("<tr>")
    for cell in cells(row)
      output = output.append("<td>#{cell}</td>")
    end
    output = output.append("</tr>")
  end
  output = output.append("</table>")
  "#{output.join("\n")}\n"
end

fn convert(source: Text) : List(Block)
  lines = source.lines
  var blocks = []
  var index = 0
  var title_seen? = false
  while index < lines.size
    line = lines.get(index) or ""
    start = index + 1
    index += 1
    if line.trim.empty?
      continue
    else if line.starts_with?("```")
      language = (line.drop_prefix("```") or "").trim
      label = if language == "mo": "ruby" else: "plain text"
      var code = ["```#{label}"]
      while index < lines.size
        next = lines.get(index) or ""
        code = code.append(next)
        index += 1
        break if next.starts_with?("```")
      end
      blocks = blocks.append(Block(text: "#{code.join("\n")}\n", line: start, heading?: false))
    else if heading?(line)
      title? = not title_seen? and line.starts_with?("# ")
      if title?
        title_seen? = true
      end
      text = if title?: "<table_of_contents/>" else: line.trim
      blocks = blocks.append(Block(text: "#{text}\n", line: start, heading?: not title?))
    else if line.starts_with?("|")
      var rows = [line]
      while index < lines.size
        next = lines.get(index) or ""
        break if not next.starts_with?("|")
        rows = rows.append(next)
        index += 1
      end
      blocks = blocks.append(Block(text: table(rows), line: start, heading?: false))
    else
      var paragraph = [line.trim]
      while index < lines.size
        next = lines.get(index) or ""
        break if next.trim.empty? or starts_block?(next)
        paragraph = paragraph.append(next.trim)
        index += 1
      end
      blocks = blocks.append(Block(text: "#{paragraph.join(" ")}\n", line: start, heading?: false))
    end
  end
  blocks
end

fn split_pieces(blocks: List(Block), limit: Int) : Result(List(Text), Block)
  for block in blocks
    return Error(block) if block.text.byte_count > limit
  end
  var pieces = []
  var current = ""
  var index = 0
  while index < blocks.size
    start = index
    index += 1
    while index < blocks.size
      next = blocks.get(index) or stop "index is inside blocks"
      break if next.heading?
      index += 1
    end
    section = blocks.drop(start).take(index - start)
    text = section.map(fn(block) block.text end).join("")
    if text.byte_count <= limit
      if current.byte_count > limit - text.byte_count
        pieces = pieces.append(current)
        current = ""
      end
      current = "#{current}#{text}"
    else
      for block in section
        if current.byte_count > limit - block.text.byte_count
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
  var a = 1
  var b = 0
  bytes = text.bytes
  for index in 0..bytes.size
    byte = bytes.get(index) or stop "index is inside bytes"
    a = (a + byte.to_int) % 65521
    b = (b + a) % 65521
  end
  var value = b * 65536 + a
  var hex = ""
  digits = "0123456789abcdef".chars
  for _ in 0..8
    digit = digits.get(value % 16) or stop "a hexadecimal digit is between 0 and 15"
    hex = "#{digit}#{hex}"
    value = value / 16
  end
  hex
end

fn piece_name(number: Int) : Text
  digits = number.to_text
  padded = if number < 10: "0#{digits}" else: digits
  "piece-#{padded}.md"
end

fn main(args: Args, console: Console, files: Files) : Int
  arguments = args.list
  if arguments.size != 2 and arguments.size != 4
    console.error("usage: notioncopy SOURCE OUT [--limit BYTES]")
    return 2
  end
  var limit = 12000
  if arguments.size == 4
    option = arguments.get(2) or ""
    amount = arguments.get(3) or ""
    if option != "--limit"
      console.error("usage: notioncopy SOURCE OUT [--limit BYTES]")
      return 2
    end
    case amount.to_int
      Some(n) if n > 0: limit = n
      Some(_) | None:
        console.error("usage: notioncopy SOURCE OUT [--limit BYTES]")
        return 2
    end
  end
  source = arguments.get(0) or ""
  output = arguments.get(1) or ""
  content = case files.read_text(source)
    Ok(text): text
    Error(problem):
      console.error("#{problem}")
      return 2
  end
  if files.exists?(output)
    case files.list(output)
      Ok(names):
        if not names.empty?
          console.error("#{output}: output folder is not empty")
          return 2
        end
      Error(problem):
        console.error("#{problem}")
        return 2
    end
  end
  pieces = case split_pieces(convert(content), limit)
    Ok(result): result
    Error(block):
      console.error(
        "#{source}:#{block.line}: a block of #{block.text.byte_count} bytes is longer than the limit"
      )
      return 1
  end
  case files.make_folder(output)
    Ok(_): Unit
    Error(problem):
      console.error("#{problem}")
      return 2
  end
  for (index, text) in pieces.enumerate
    name = piece_name(index + 1)
    case files.write_text("#{output}/#{name}", text)
      Ok(_): console.print("#{name} #{text.byte_count} #{checksum(text)}")
      Error(problem):
        console.error("#{problem}")
        return 2
    end
  end
  0
end

test "paragraphs, list continuations, and code preserve their block boundaries"
  source = "# Title\n\n one \n two \n- item\n  continued\n## Next\n```mo\n# raw\n\n```\n"
  blocks = convert(source)
  assert blocks.map(fn(block) block.text end).join("") ==
    "<table_of_contents/>\none two\n- item continued\n## Next\n```ruby\n# raw\n\n```\n"
  assert blocks.map(fn(block) block.line end) == [1, 3, 5, 7, 8]
end
