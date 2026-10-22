## Checks that the brackets in each file named on the command line balance.
module Brackets

## A bracket and where it is: its line and column, counted from 1.
struct Spot
  bracket: Char
  line: Int
  column: Int
end

## The first thing wrong with a file's brackets.
enum Problem
  Unexpected(close: Spot)
  Mismatched(close: Spot, open: Spot)
  NeverClosed(open: Spot)
end

fn closer(open: Char) : Char
  examples
    closer('(') == ')'
    closer('{') == '}'
  end

  case open
    '(': ')'
    '[': ']'
    _: '}'
  end
end

## The first problem with the brackets in `text`, or `None` when they balance.
fn problem(text: Text) : Option(Problem)
  examples
    problem("f(a[1], {b})") == None
    problem("a)") == Some(Unexpected(close: Spot(bracket: ')', line: 1, column: 2)))
    problem("x{\n(\n)") == Some(NeverClosed(open: Spot(bracket: '{', line: 1, column: 2)))
  end

  var open = []
  var line = 1
  var column = 0
  for c in text.chars
    column += 1
    if c == '\n'
      line += 1
      column = 0
    else if c == '(' or c == '[' or c == '{'
      open = open.append(Spot(bracket: c, line: line, column: column))
    else if c == ')' or c == ']' or c == '}'
      close = Spot(bracket: c, line: line, column: column)
      case open.last
        None: return Some(Unexpected(close: close))
        Some(innermost) if closer(innermost.bracket) != c: return Some(
          Mismatched(close: close, open: innermost)
        )
        Some(_): open = open.take(open.size - 1)
      end
    end
  end
  open.first.map(fn(innermost) NeverClosed(open: innermost) end)
end

## The line reporting `problem` in the file `name`.
fn report(name: Text, problem: Problem) : Text
  examples
    report("a.txt", Unexpected(close: Spot(bracket: ']', line: 3, column: 7))) ==
      "a.txt:3:7: unexpected ]"
    report(
      "a.txt",
      Mismatched(
        close: Spot(bracket: ']', line: 2, column: 2),
        open: Spot(bracket: '(', line: 1, column: 1)
      )
    ) ==
      "a.txt:2:2: ] does not match ( at 1:1"
    report("a.txt", NeverClosed(open: Spot(bracket: '{', line: 1, column: 2))) ==
      "a.txt: { at 1:2 is never closed"
  end

  case problem
    Unexpected(close: c): "#{name}:#{c.line}:#{c.column}: unexpected #{c.bracket}"
    Mismatched(close: c, open: o): "#{name}:#{c.line}:#{c.column}: #{c.bracket} does not match #{o
      .bracket} at #{o.line}:#{o.column}"
    NeverClosed(open: o): "#{name}: #{o.bracket} at #{o.line}:#{o.column} is never closed"
  end
end

fn main(args: Args, console: Console, files: ReadFiles) : Int
  if args.list.empty?
    console.error("usage: brackets FILE...")
    return 2
  end
  var status = 0
  for name in args.list
    case files.read_text(name)
      Ok(text):
        case problem(text)
          None: console.print("#{name}: ok")
          Some(found):
            console.print(report(name, found))
            status = 1
        end
      Error(failure):
        console.error("brackets: #{failure}")
        status = 1
    end
  end
  status
end

test "each file is checked, and a missing one reported"
  console = Console.fake()
  files = Files.in_memory({"good.txt": "(ok)", "bad.txt": "(]"}).read_only
  assert main(Args.of(["good.txt", "gone.txt", "bad.txt"]), console, files) == 1
  assert console.printed == ["good.txt: ok", "bad.txt:1:2: ] does not match ( at 1:1"]
  assert console.printed_errors == ["brackets: gone.txt: no such file"]
end
