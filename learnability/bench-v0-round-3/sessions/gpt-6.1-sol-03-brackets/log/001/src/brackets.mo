## Checks brackets in each file, ignoring all other characters.
module Brackets

struct Opening
  bracket: Char
  line: Int
  column: Int
end

fn problem(path: Text, text: Text) : Option(Text)
  var stack = []
  var line = 1
  var column = 1
  for character in text.chars
    if character == '(' or character == '[' or character == '{'
      stack = stack.append(Opening(bracket: character, line: line, column: column))
    else if character == ')' or character == ']' or character == '}'
      case stack.last
        None: return Some("#{path}:#{line}:#{column}: unexpected #{character}")
        Some(opening):
          matches? = (character == ')' and opening.bracket == '(') or (
            character == ']' and opening.bracket == '['
          ) or (character == '}' and opening.bracket == '{')
          if not matches?
            return Some(
              "#{path}:#{line}:#{column}: #{character} does not match #{opening.bracket} at #{opening
                .line}:#{opening.column}"
            )
          end
          stack = stack.take(stack.size - 1)
      end
    end
    if character == '\n'
      line += 1
      column = 1
    else
      column += 1
    end
  end
  case stack.last
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
      Error(error):
        console.error("brackets: #{error}")
        status = 1
      Ok(text):
        case problem(path, text)
          None: console.print("#{path}: ok")
          Some(message):
            console.print(message)
            status = 1
        end
    end
  end
  status
end

test "the supplied example reports the first mismatch and checks both files"
  console = Console.fake()
  files = Files.in_memory({
    "main.c": "int main() {\n  return f(a[0]);\n}\n",
    "notes.txt": "a (b\nc] d\n"
  }).read_only
  assert main(Args.of(["main.c", "notes.txt"]), console, files) == 1
  assert console.printed == ["main.c: ok", "notes.txt:2:2: ] does not match ( at 1:3"]
  assert console.printed_errors == []
end
