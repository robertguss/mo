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

test "each file reports its first problem and checking continues after read errors"
  console = Console.fake()
  files = Files.in_memory({
    "notes.txt": "a (b\nc] d\n",
    "crossed": "([)]",
    "unicode": "é😀e\u{301}\t\r){",
    "unclosed": "(\n é[{}",
    "first": ")(",
    "empty": "",
    "plain": "just text\n"
  }).read_only
  assert main(
    Args.of(["notes.txt", "missing", "crossed", "unicode", "unclosed", "first", "empty", "plain"]),
    console,
    files
  ) ==
    1
  assert console.printed == [
    "notes.txt:2:2: ] does not match ( at 1:3",
    "crossed:1:3: ) does not match [ at 1:2",
    "unicode:1:7: unexpected )",
    "unclosed: [ at 2:3 is never closed",
    "first:1:1: unexpected )",
    "empty: ok",
    "plain: ok"
  ]
  assert console.printed_errors == ["brackets: missing: no such file"]
end

test "brackets inside quotes and comments are still checked"
  console = Console.fake()
  files = Files.in_memory({"quoted": "\"[\"", "comment": "# ]"}).read_only
  assert main(Args.of(["quoted", "comment"]), console, files) == 1
  assert console.printed == ["quoted: [ at 1:2 is never closed", "comment:1:3: unexpected ]"]
  assert console.printed_errors == []
end

test "every closing kind must match the innermost opening kind"
  for (text, expected) in [
    ("(]", "] does not match ("),
    ("(}", "} does not match ("),
    ("[)", ") does not match ["),
    ("[}", "} does not match ["),
    ("{)", ") does not match {"),
    ("{]", "] does not match {")
  ]
    console = Console.fake()
    files = Files.in_memory({"pair": text}).read_only
    assert main(Args.of(["pair"]), console, files) == 1
    assert console.printed == ["pair:1:2: #{expected} at 1:1"]
    assert console.printed_errors == []
  end
end

test "no arguments prints usage only and exits with status two"
  console = Console.fake()
  files = Files.in_memory({}).read_only
  assert main(Args.of([]), console, files) == 2
  assert console.printed == []
  assert console.printed_errors == ["usage: brackets FILE..."]
end
