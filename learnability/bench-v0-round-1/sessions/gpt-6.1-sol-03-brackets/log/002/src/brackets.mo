## Checks bracket nesting in each file named on the command line.
module Brackets

struct Opening
  bracket: Char
  line: Int
  column: Int
end

fn inspect(path: Text, text: Text) : Option(Text)
  var openings = []
  var line = 1
  var column = 1
  for character in text.chars
    case character
      '(' | '[' | '{': openings = openings.append(
        Opening(bracket: character, line: line, column: column)
      )
      ')' | ']' | '}':
        case openings.last
          None: return Some("#{path}:#{line}:#{column}: unexpected #{character}")
          Some(opening):
            matches? = (opening.bracket == '(' and character == ')') or (
              opening.bracket == '[' and character == ']'
            ) or (opening.bracket == '{' and character == '}')
            if not matches?
              return Some(
                "#{path}:#{line}:#{column}: #{character} does not match #{opening.bracket} at #{opening
                  .line}:#{opening.column}"
              )
            end
            openings = openings.take(openings.size - 1)
        end
      _: Unit
    end
    if character == '\n'
      line += 1
      column = 1
    else
      column += 1
    end
  end
  case openings.last
    None: None
    Some(opening): Some(
      "#{path}: #{opening.bracket} at #{opening.line}:#{opening.column} is never closed"
    )
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
      Error(problem):
        console.error("brackets: #{problem}")
        status = 1
      Ok(text):
        case inspect(path, text)
          None: console.print("#{path}: ok")
          Some(problem):
            console.print(problem)
            status = 1
        end
    end
  end
  status
end

test "nested brackets balance across lines"
  console = Console.fake()
  files = Files.in_memory({"main.c": "int main() {\n  return f(a[0]);\n}\n"}).read_only
  assert main(Args.of(["main.c"]), console, files) == 0
  assert console.printed == ["main.c: ok"]
  assert console.printed_errors == []
end
