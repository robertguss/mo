## Checks that the brackets in each file named on the command line balance.
module Brackets

## A bracket that is still open, and where it is.
struct Open
  bracket: Char
  line: Int
  column: Int
end

## What reading a text's brackets finds.
enum Outcome
  Balanced
  Unexpected(closing: Char, line: Int, column: Int)
  Mismatch(closing: Char, line: Int, column: Int, open: Open)
  Unclosed(open: Open)
end

## The opening bracket that a closing bracket closes, or None for any other character.
fn opener(c: Char) : Option(Char)
  examples
    opener(')') == Some('(')
    opener(']') == Some('[')
    opener('}') == Some('{')
    opener('(') == None
    opener('a') == None
  end

  case c
    ')': Some('(')
    ']': Some('[')
    '}': Some('{')
    _: None
  end
end

fn opening?(c: Char) : Bool
  c == '(' or c == '[' or c == '{'
end

fn count(text: Text, c: Char) : Int
  text.chars.count(fn(x) x == c end)
end

## Reads the text from its start, and gives the first problem with its brackets.
fn check(text: Text) : Outcome
  examples
    check("") == Balanced
    check("int main() {\n  return f(a[0]);\n}\n") == Balanced
    check("a (b\nc] d\n") == Mismatch(
      closing: ']',
      line: 2,
      column: 2,
      open: Open(bracket: '(', line: 1, column: 3)
    )
    check("x)") == Unexpected(closing: ')', line: 1, column: 2)
    check("é}") == Unexpected(closing: '}', line: 1, column: 2)
    check("( [\n{") == Unclosed(open: Open(bracket: '{', line: 2, column: 1))
    check("(]") == Mismatch(
      closing: ']',
      line: 1,
      column: 2,
      open: Open(bracket: '(', line: 1, column: 1)
    )
  end
  promise "balanced text has as many closing brackets of each kind as opening ones"
    result == Balanced implies count(text, '(') == count(text, ')') and
      count(text, '[') == count(text, ']') and
      count(text, '{') == count(text, '}')
    allows text: "(a)", result: Balanced
    forbids text: "((", result: Balanced
  end

  var stack = []
  var line = 1
  var column = 0
  for c in text.chars
    if c == '\n'
      line += 1
      column = 0
      continue
    end
    column += 1
    if opening?(c)
      stack = stack.append(Open(bracket: c, line: line, column: column))
    else if opener(c) is Some(wanted)
      case stack.last
        None: return Unexpected(closing: c, line: line, column: column)
        Some(top):
          return Mismatch(closing: c, line: line, column: column, open: top) if top.bracket != wanted
          stack = stack.take(stack.size - 1)
      end
    end
  end
  case stack.last
    None: Balanced
    Some(top): Unclosed(open: top)
  end
end

## The line printed for a file whose brackets were checked.
fn report(path: Text, outcome: Outcome) : Text
  examples
    report("main.c", Balanced) == "main.c: ok"
    report("a", Unexpected(closing: ')', line: 3, column: 4)) == "a:3:4: unexpected )"
    report(
      "notes.txt",
      Mismatch(closing: ']', line: 2, column: 2, open: Open(bracket: '(', line: 1, column: 3))
    ) == "notes.txt:2:2: ] does not match ( at 1:3"
    report("b", Unclosed(open: Open(bracket: '{', line: 5, column: 1))) == "b: { at 5:1 is never closed"
  end

  case outcome
    Balanced: "#{path}: ok"
    Unexpected(closing: c, line: l, column: col): "#{path}:#{l}:#{col}: unexpected #{c}"
    Mismatch(closing: c, line: l, column: col, open: o):
      "#{path}:#{l}:#{col}: #{c} does not match #{o.bracket} at #{o.line}:#{o.column}"
    Unclosed(open: o): "#{path}: #{o.bracket} at #{o.line}:#{o.column} is never closed"
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
      Ok(text):
        outcome = check(text)
        console.print(report(path, outcome))
        status = 1 if outcome != Balanced
      Error(e):
        console.error("brackets: #{e}")
        status = 1
    end
  end
  status
end

test "the example from the task"
  console = Console.fake()
  files = Files.in_memory({
    "main.c": "int main() {\n  return f(a[0]);\n}\n",
    "notes.txt": "a (b\nc] d\n"
  }).read_only
  assert main(Args.of(["main.c", "notes.txt"]), console, files) == 1
  assert console.printed == ["main.c: ok", "notes.txt:2:2: ] does not match ( at 1:3"]
  assert console.printed_errors == []
end

test "a missing file is reported and the next file is still checked"
  console = Console.fake()
  files = Files.in_memory({"dir/ok.txt": "[()]{}"}).read_only
  assert main(Args.of(["gone.txt", "dir/ok.txt"]), console, files) == 1
  assert console.printed == ["dir/ok.txt: ok"]
  assert console.printed_errors == ["brackets: gone.txt: no such file"]
end

test "all balanced files exit with 0"
  console = Console.fake()
  files = Files.in_memory({"a": "(", "b": "x"}).read_only
  assert main(Args.of(["b"]), console, files) == 0
  assert console.printed == ["b: ok"]
end

test "no arguments prints usage and exits with 2"
  console = Console.fake()
  files = Files.in_memory({}).read_only
  assert main(Args.of([]), console, files) == 2
  assert console.printed == []
  assert console.printed_errors == ["usage: brackets FILE..."]
end
