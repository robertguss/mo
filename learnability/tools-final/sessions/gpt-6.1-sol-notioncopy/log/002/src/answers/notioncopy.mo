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

fn main(args: Args, console: Console, files: Files) : Int
  if args.list.size != 2
    console.error("usage: notioncopy SOURCE OUT [--limit BYTES]")
    return 2
  end
  source = args.list.get(0) or ""
  output = args.list.get(1) or ""
  content = case files.read_text(source)
    Ok(text): text
    Error(problem):
      console.error("#{problem}")
      return 2
  end
  text = convert(content).map(fn(block) block.text end).join("")
  case files.make_folder(output)
    Ok(_): Unit
    Error(problem):
      console.error("#{problem}")
      return 2
  end
  case files.write_text("#{output}/piece-01.md", text)
    Ok(_): 0
    Error(problem):
      console.error("#{problem}")
      2
  end
end

test "paragraphs, list continuations, and code preserve their block boundaries"
  source = "# Title\n\n one \n two \n- item\n  continued\n## Next\n```mo\n# raw\n\n```\n"
  blocks = convert(source)
  assert blocks.map(fn(block) block.text end).join("") ==
    "<table_of_contents/>\none two\n- item continued\n## Next\n```ruby\n# raw\n\n```\n"
  assert blocks.map(fn(block) block.line end) == [1, 3, 5, 7, 8]
end
