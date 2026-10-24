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
