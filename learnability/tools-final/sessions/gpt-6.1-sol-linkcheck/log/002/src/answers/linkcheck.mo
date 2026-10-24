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

fn without_spans(text: Text) : Text
  chars = text.chars
  var output = []
  var index = 0
  while index < chars.size
    ch = char_at(chars, index)
    if ch == '\\' and index + 1 < chars.size
      output = output.append(ch).append(char_at(chars, index + 1))
      index += 2
    else if ch == '`'
      after = run_end(chars, index)
      var close = after
      var found? = false
      while close < chars.size
        if char_at(chars, close) == '`'
          next = run_end(chars, close)
          if next - close == after - index
            close = next
            found? = true
            break
          end
          close = next
        else
          close += 1
        end
      end
      if found?
        for position in index..close
          output = output.append(if char_at(chars, position) == '\n': '\n' else: ' ')
        end
        index = close
      else
        output = output.concat(chars.drop(index).take(after - index))
        index = after
      end
    else
      output = output.append(ch)
      index += 1
    end
  end
  Text.from_chars(output)
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
  visible = without_spans(without_blocks(text))
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

fn main(args: Args, console: Console, files: ReadFiles) : Int
  if args.list.size > 1
    console.error("usage: linkcheck [FOLDER]")
    return 2
  end
  folder = args.list.first or "."
  case files.entries(folder)
    Ok(entries):
      console.print("ok: 0 links in #{entries.size} files")
      0
    Error(problem):
      console.error("#{problem}")
      2
  end
end
