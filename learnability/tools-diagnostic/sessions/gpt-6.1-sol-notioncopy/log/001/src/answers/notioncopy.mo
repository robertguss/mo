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

fn main(args: Args, console: Console, files: Files) : Int
  if args.list.size != 2
    console.error("usage: notioncopy SOURCE OUT [--limit BYTES]")
    return 2
  end
  source = args.list.get(0) or ""
  out = args.list.get(1) or ""
  text = case files.read_text(source)
    Ok(value): value
    Error(problem):
      console.error("#{problem}")
      return 2
  end
  blocks = convert(text)
  console.print("#{out}: #{blocks.size} blocks")
  0
end

test "paragraphs, list continuations and the first title are converted"
  blocks = convert("# Title\n\n One \n two \n\n- item\n continued\n# Later\n")
  assert blocks.map(fn(b) b.text end).join("") ==
    "<table_of_contents/>\nOne two\n- item continued\n# Later\n"
end
