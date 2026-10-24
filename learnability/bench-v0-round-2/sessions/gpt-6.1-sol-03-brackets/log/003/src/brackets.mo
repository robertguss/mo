## Checks bracket nesting in each file named on the command line.
module Brackets

struct Opening
  bracket: Char
  line: Int
  column: Int
end

fn problem(text: Text) : Option(Text)
  var stack = []
  var line = 1
  var column = 1
  for c in text.chars
    case c
      '(' | '[' | '{': stack = stack.append(Opening(bracket: c, line: line, column: column))
      ')' | ']' | '}':
        case stack.last
          None: return Some(":#{line}:#{column}: unexpected #{c}")
          Some(opening):
            matches? = (opening.bracket == '(' and c == ')') or (
              opening.bracket == '[' and c == ']'
            ) or (opening.bracket == '{' and c == '}')
            if not matches?
              return Some(
                ":#{line}:#{column}: #{c} does not match #{opening.bracket} at #{opening.line}:#{opening
                  .column}"
              )
            end
            stack = stack.take(stack.size - 1)
        end
      _: Unit
    end
    if c == '\n'
      line += 1
      column = 1
    else
      column += 1
    end
  end
  case stack.last
    None: None
    Some(opening): Some(": #{opening.bracket} at #{opening.line}:#{opening.column} is never closed")
  end
end

fn main(args: Args, console: Console, files: ReadFiles) : Int
  if args.list.empty?
    console.error("usage: brackets FILE...")
    return 2
  end
  var status = 0
  for path in args.list
    case files.read_text(path)
      Error(error):
        console.error("brackets: #{error}")
        status = 1
      Ok(text):
        case problem(text)
          None: console.print("#{path}: ok")
          Some(detail):
            console.print("#{path}#{detail}")
            status = 1
        end
    end
  end
  status
end

test "the sample reports the first mismatch and preserves file order"
  console = Console.fake()
  files = Files.in_memory({
    "main.c": "int main() {\n  return f(a[0]);\n}\n",
    "notes.txt": "a (b\nc] d\n"
  }).read_only
  assert main(Args.of(["main.c", "notes.txt"]), console, files) == 1
  assert console.printed == ["main.c: ok", "notes.txt:2:2: ] does not match ( at 1:3"]
  assert console.printed_errors == []
end

test "no arguments prints usage only on standard error"
  console = Console.fake()
  files = Files.in_memory({}).read_only
  assert main(Args.of([]), console, files) == 2
  assert console.printed == []
  assert console.printed_errors == ["usage: brackets FILE..."]
end

test "empty files and nested or adjacent pairs succeed"
  console = Console.fake()
  files = Files.in_memory({"empty": "", "plain": "é😀 words\r\n\t", "nested": "{a[(b)]} () [] {}\n"})
    .read_only
  assert main(Args.of(["empty", "plain", "nested", "empty"]), console, files) == 0
  assert console.printed == ["empty: ok", "plain: ok", "nested: ok", "empty: ok"]
  assert console.printed_errors == []
end

test "missing files do not stop later files or erase an earlier failure"
  console = Console.fake()
  files = Files.in_memory({"good": "()", "bad": "]"}).read_only
  assert main(Args.of(["bad", "missing", "good"]), console, files) == 1
  assert console.printed == ["bad:1:1: unexpected ]", "good: ok"]
  assert console.printed_errors == ["brackets: missing: no such file"]
end

test "only the first problem and the innermost remaining opening are reported"
  console = Console.fake()
  files = Files.in_memory({
    "crossed": "([)]}",
    "round": ")][",
    "curly": "{} } (",
    "unfinished": "{\n  [()",
    "popped": "{[()]"
  }).read_only
  assert main(Args.of(["crossed", "round", "curly", "unfinished", "popped"]), console, files) == 1
  assert console.printed == [
    "crossed:1:3: ) does not match [ at 1:2",
    "round:1:1: unexpected )",
    "curly:1:4: unexpected }",
    "unfinished: [ at 2:3 is never closed",
    "popped: { at 1:1 is never closed"
  ]
  assert console.printed_errors == []
end

test "positions count code points and only newline ends a line"
  console = Console.fake()
  files = Files.in_memory({
    "unicode": "é😀(\r\n\t界]}",
    "carriage": "\r\t)",
    "combining": "e\u{301}]",
    "newline": "\n\n]"
  }).read_only
  assert main(Args.of(["unicode", "carriage", "combining", "newline"]), console, files) == 1
  assert console.printed == [
    "unicode:2:3: ] does not match ( at 1:3",
    "carriage:1:3: unexpected )",
    "combining:1:3: unexpected ]",
    "newline:3:1: unexpected ]"
  ]
  assert console.printed_errors == []
end

test "brackets inside strings and comments still count"
  console = Console.fake()
  files = Files.in_memory({"string": "\"(\"", "comment": "// ]"}).read_only
  assert main(Args.of(["string", "comment"]), console, files) == 1
  assert console.printed == ["string: ( at 1:2 is never closed", "comment:1:4: unexpected ]"]
  assert console.printed_errors == []
end
