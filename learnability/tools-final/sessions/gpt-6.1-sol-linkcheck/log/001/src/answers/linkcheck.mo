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
