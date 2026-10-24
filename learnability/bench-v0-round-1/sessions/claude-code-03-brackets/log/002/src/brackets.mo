## Checks that the brackets in each file named on the command line balance.
module Brackets

## A place in a file, counting lines and characters from 1.
struct Place
  line: Int
  column: Int
end

## What reading a text's brackets found: that they balance, or the first problem.
enum Finding
  Balanced
  Unexpected(close: Char, at: Place)
  Mismatch(close: Char, at: Place, open: Char, opened: Place)
  Unclosed(open: Char, at: Place)
end

## The opening bracket that a closing bracket closes, or None for any other character.
fn opener(c: Char) : Option(Char)
  examples
    opener(')') == Some('(')
    opener(']') == Some('[')
    opener('}') == Some('{')
    opener('(') == None
    opener('x') == None
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

fn bracket?(c: Char) : Bool
  opening?(c) or opener(c) != None
end

## The first problem with the brackets in `text`, or Balanced.
fn check(text: Text) : Finding
  examples
    check("") == Balanced
    check("int main() {\n  return f(a[0]);\n}\n") == Balanced
    check("a (b\nc] d\n") ==
      Mismatch(
        close: ']',
        at: Place(line: 2, column: 2),
        open: '(',
        opened: Place(line: 1, column: 3)
      )
    check("x)") == Unexpected(close: ')', at: Place(line: 1, column: 2))
    check("([{\n") == Unclosed(open: '{', at: Place(line: 1, column: 3))
    check("é(é]") ==
      Mismatch(
        close: ']',
        at: Place(line: 1, column: 4),
        open: '(',
        opened: Place(line: 1, column: 2)
      )
    check("\r\n)") == Unexpected(close: ')', at: Place(line: 2, column: 1))
    check("() ]") == Unexpected(close: ']', at: Place(line: 1, column: 4))
  end
  promise "a text with no brackets in it balances"
    text.chars.any?(bracket?) or result == Balanced
    allows text: "a b", result: Balanced
    forbids text: "a b", result: Unclosed(open: '(', at: Place(line: 1, column: 1))
  end

  var open = []
  var line = 1
  var column = 0
  for c in text.chars
    if c == '\n'
      line += 1
      column = 0
      continue
    end
    column += 1
    here = Place(line: line, column: column)
    if opening?(c)
      open = open.append((c, here))
    else if opener(c) is Some(wanted)
      case open.last
        None: return Unexpected(close: c, at: here)
        Some((o, opened)):
          return Mismatch(close: c, at: here, open: o, opened: opened) if o != wanted
          open = open.take(open.size - 1)
      end
    end
  end
  case open.last
    None: Balanced
    Some((o, opened)): Unclosed(open: o, at: opened)
  end
end

## The line printed for the file at `path`.
fn report(path: Text, finding: Finding) : Text
  examples
    report("main.c", Balanced) == "main.c: ok"
    report("a.txt", Unexpected(close: ')', at: Place(line: 3, column: 7))) ==
      "a.txt:3:7: unexpected )"
    report(
      "notes.txt",
      Mismatch(
        close: ']',
        at: Place(line: 2, column: 2),
        open: '(',
        opened: Place(line: 1, column: 3)
      )
    ) ==
      "notes.txt:2:2: ] does not match ( at 1:3"
    report("b", Unclosed(open: '{', at: Place(line: 4, column: 1))) == "b: { at 4:1 is never closed"
  end

  case finding
    Balanced: "#{path}: ok"
    Unexpected(close: c, at: at): "#{path}:#{at.line}:#{at.column}: unexpected #{c}"
    Mismatch(close: c, at: at, open: o, opened: opened): "#{path}:#{at.line}:#{at.column}: #{c} does not match #{o} at #{opened
      .line}:#{opened.column}"
    Unclosed(open: o, at: at): "#{path}: #{o} at #{at.line}:#{at.column} is never closed"
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
        finding = check(text)
        console.print(report(path, finding))
        if finding != Balanced
          status = 1
        end
      Error(problem):
        console.error("brackets: #{problem}")
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
  files = Files.in_memory({"src/a.mo": "{[()]}"}).read_only
  assert main(Args.of(["gone.txt", "src/a.mo"]), console, files) == 1
  assert console.printed == ["src/a.mo: ok"]
  assert console.printed_errors == ["brackets: gone.txt: no such file"]
end

test "every file balancing exits 0"
  console = Console.fake()
  files = Files.in_memory({"a": "(x)", "b": "none"}).read_only
  assert main(Args.of(["a", "b"]), console, files) == 0
  assert console.printed == ["a: ok", "b: ok"]
end

test "no arguments prints the usage and exits 2"
  console = Console.fake()
  files = Files.in_memory({}).read_only
  assert main(Args.of([]), console, files) == 2
  assert console.printed == []
  assert console.printed_errors == ["usage: brackets FILE..."]
end
