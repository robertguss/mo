## A to-do list kept in the file todo.txt, in the folder the program runs in.
##
##   todo add TEXT     adds an item
##   todo list         lists the items, numbered from 1
##   todo done N       marks item N done
##   todo remove N     removes item N
module Todo

file = "todo.txt"

struct Item
  text: Text
  done?: Bool
end

enum Command
  Add(text: Text)
  Show
  Done(number: Int)
  Remove(number: Int)
end

enum TodoError
  Usage
  BadLine(number: Int)
  NoSuchItem(number: Int)
  Storage(FileError)
end

fn to_text(problem: TodoError) : Text
  case problem
    Usage: "usage: todo add TEXT | list | done N | remove N"
    BadLine(number: n): "line #{n} of #{file} isn't an item"
    NoSuchItem(number: n): "there is no item #{n}"
    Storage(failure): "#{failure}"
  end
end

fn parse_command(args: List(Text)) : Option(Command)
  examples
    parse_command(["add", "buy", "milk"]) == Some(Add(text: "buy milk"))
    parse_command(["list"]) == Some(Show)
    parse_command(["done", "2"]) == Some(Done(number: 2))
    parse_command(["done", "two"]) == None
    parse_command(["add"]) == None
  end

  case args
    ["add", ..words] if not words.empty?: Some(Add(text: words.join(" ")))
    ["list"]: Some(Show)
    ["done", n]: Some(Done(number: try n.to_int))
    ["remove", n]: Some(Remove(number: try n.to_int))
    _: None
  end
end

## The items in the file's text: one a line, "[ ] text" or "[x] text".
fn parse_items(text: Text) : Result(List(Item), TodoError)
  examples
    parse_items("[ ] milk\n[x] eggs\n") == Ok([Item(text: "milk", done?: false), Item(text: "eggs", done?: true)])
    parse_items("") == Ok([])
    parse_items("milk\n") == Error(BadLine(number: 1))
  end

  var items = []
  for (index, line) in text.lines.enumerate
    item = case line.drop_prefix("[x] ")
      Some(rest): Item(text: rest, done?: true)
      None:
        rest = try line.drop_prefix("[ ] ").or_fail(BadLine(number: index + 1))
        Item(text: rest, done?: false)
    end
    items = items.append(item)
  end
  Ok(items)
end

fn format(items: List(Item)) : Text
  examples
    format([Item(text: "milk", done?: false), Item(text: "eggs", done?: true)]) == "[ ] milk\n[x] eggs\n"
  end

  items.map(fn(item) "#{box(item)} #{item.text}\n" end).join("")
end

fn box(item: Item) : Text
  if item.done?: "[x]" else: "[ ]"
end

fn apply(items: List(Item), command: Command) : Result(List(Item), TodoError)
  promise "marking an item done keeps every item"
    command is Done(number: _) and result is Ok(after) implies after.size == items.size
    allows items: [milk], command: Done(number: 1), result: Ok([milk_done])
    forbids items: [milk], command: Done(number: 1), result: Ok([])
  end

  examples
    apply([milk], Done(number: 1)) == Ok([milk_done])
    apply([milk], Remove(number: 0)) == Error(NoSuchItem(number: 0))
    apply([milk], Remove(number: 2)) == Error(NoSuchItem(number: 2))
  end

  case command
    Add(text: text): Ok(items.append(Item(text: text, done?: false)))
    Show: Ok(items)
    Done(number: n):
      return Error(NoSuchItem(number: n)) if n < 1
      var item = try items.get(n - 1).or_fail(NoSuchItem(number: n))
      item.done? = true
      items.set(n - 1, item).or_fail(NoSuchItem(number: n))
    Remove(number: n):
      return Error(NoSuchItem(number: n)) if n < 1
      items.remove_at(n - 1).or_fail(NoSuchItem(number: n))
  end
end

fn main(args: Args, console: Console, files: Files) : Result(Unit, TodoError)
  command = try parse_command(args.list).or_fail(Usage)
  saved = case files.read_text(file)
    Ok(text): text
    Error(NotFound(path: _)): ""
    Error(failure): return Error(Storage(failure))
  end
  items = try apply(try parse_items(saved), command)
  if command == Show
    for (index, item) in items.enumerate
      console.print("#{index + 1}. #{box(item)} #{item.text}")
    end
  else
    try files.write_text(file, format(items))
  end
  Ok(Unit)
end

milk = Item(text: "milk", done?: false)
milk_done = Item(text: "milk", done?: true)

test "items added and marked done are saved, and listed by number"
  files = Files.in_memory({})
  console = Console.fake()
  assert main(Args.of(["add", "buy", "milk"]), console, files) == Ok(Unit)
  assert main(Args.of(["add", "call", "Ana"]), console, files) == Ok(Unit)
  assert main(Args.of(["done", "1"]), console, files) == Ok(Unit)
  assert files.read_text(file) == Ok("[x] buy milk\n[ ] call Ana\n")
  assert main(Args.of(["list"]), console, files) == Ok(Unit)
  assert console.printed == ["1. [x] buy milk", "2. [ ] call Ana"]
end

test "an item that isn't there is reported"
  files = Files.in_memory({file: "[ ] milk\n"})
  assert main(Args.of(["remove", "3"]), Console.fake(), files) == Error(NoSuchItem(number: 3))
end
