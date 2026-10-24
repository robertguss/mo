## Checks that the brackets in each file named on the command line balance.
module Brackets

## An opening bracket and where it is.
struct Open
  bracket: Char
  line: Int
  column: Int
end

## What reading a text's brackets found: balance, or the first problem.
enum Problem
  Balanced
  Unexpected(found: Char, line: Int, column: Int)
  Mismatch(found: Char, line: Int, column: Int, open: Open)
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

## Whether a character opens a bracket.
fn opening?(c: Char) : Bool
  c == '(' or c == '[' or c == '{'
end

## Reads the text from its start and gives the first problem with its brackets.
fn check(text: Text) : Problem
  examples
    check("") == Balanced
    check("int main() {\n  return f(a[0]);\n}\n") == Balanced
    check("a (b\nc] d\n") ==
      Mismatch(found: ']', line: 2, column: 2, open: Open(bracket: '(', line: 1, column: 3))
    check("x)") == Unexpected(found: ')', line: 1, column: 2)
    check("{ [ (\n") == Unclosed(open: Open(bracket: '(', line: 1, column: 5))
    check("é(") == Unclosed(open: Open(bracket: '(', line: 1, column: 2))
  end
  promise "a text with no brackets in it is balanced"
    text.chars.any?(fn(c) opening?(c) or opener(c) != None end) or result == Balanced
    allows text: "ab", result: Balanced
    forbids text: "ab", result: Unexpected(found: ')', line: 1, column: 1)
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
        None: return Unexpected(found: c, line: line, column: column)
        Some(open):
          return Mismatch(found: c, line: line, column: column, open: open) if open.bracket !=
            wanted
          stack = stack.take(stack.size - 1)
      end
    end
  end
  case stack.last
    None: Balanced
    Some(open): Unclosed(open: open)
  end
end

## The line printed for a file, given its path and what checking it found.
fn report(path: Text, problem: Problem) : Text
  examples
    report("main.c", Balanced) == "main.c: ok"
    report("a.txt", Unexpected(found: '}', line: 3, column: 4)) == "a.txt:3:4: unexpected }"
    report(
      "n.txt",
      Mismatch(found: ']', line: 2, column: 2, open: Open(bracket: '(', line: 1, column: 3))
    ) ==
      "n.txt:2:2: ] does not match ( at 1:3"
    report("b.txt", Unclosed(open: Open(bracket: '[', line: 5, column: 1))) ==
      "b.txt: [ at 5:1 is never closed"
  end

  case problem
    Balanced: "#{path}: ok"
    Unexpected(found: c, line: l, column: col): "#{path}:#{l}:#{col}: unexpected #{c}"
    Mismatch(found: c, line: l, column: col, open: o): "#{path}:#{l}:#{col}: #{c} does not match #{o
      .bracket} at #{o.line}:#{o.column}"
    Unclosed(open: o): "#{path}: #{o.bracket} at #{o.line}:#{o.column} is never closed"
  end
end

fn main(args: Args, console: Console, files: ReadFiles) : Int
  paths = args.list
  if paths.empty?
    console.error("usage: brackets FILE...")
    return 2
  end
  var status = 0
  for path in paths
    case files.read_text(path)
      Ok(text):
        problem = check(text)
        console.print(report(path, problem))
        if problem != Balanced
          status = 1
        end
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

test "a missing file is reported on standard error and the next file is still checked"
  console = Console.fake()
  files = Files.in_memory({"ok.txt": "([]{})"}).read_only
  assert main(Args.of(["gone.txt", "ok.txt"]), console, files) == 1
  assert console.printed == ["ok.txt: ok"]
  assert console.printed_errors == ["brackets: gone.txt: no such file"]
end

test "every file balancing exits with status 0"
  console = Console.fake()
  files = Files.in_memory({"a": "(x)", "d/b": "no brackets"}).read_only
  assert main(Args.of(["a", "d/b"]), console, files) == 0
  assert console.printed == ["a: ok", "d/b: ok"]
end

test "no arguments prints the usage and exits with status 2"
  console = Console.fake()
  files = Files.in_memory({}).read_only
  assert main(Args.of([]), console, files) == 2
  assert console.printed == []
  assert console.printed_errors == ["usage: brackets FILE..."]
end
