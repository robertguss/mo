## todo: a to-do list kept in the file todo.txt, in the folder the program is
## started in.
##
##   todo add TEXT    adds an item to the end of the list
##   todo list        shows the list, numbered from 1
##   todo done N      marks item N as done
##   todo remove N    removes item N; the items after it move up by one
##
## Each line of todo.txt is one item: "[ ] " and its text for an item still
## to do, or "[x] " and its text for one that is done.
module Todo

## The file that holds the list.
todo_file = "todo.txt"

## The marks that start an item's line in the file.
done_mark = "[x]"
open_mark = "[ ]"

## What `todo help` prints, and what follows a mistake on the command line.
usage = """
  usage:
    todo add TEXT    add an item to the end of the list
    todo list        show the list, numbered from 1
    todo done N      mark item N as done
    todo remove N    remove item N from the list
  The list is kept in todo.txt, in the current folder.
  """

## One entry in the list.
struct Item
  text: Text
  done?: Bool
end

## What the command line asks for.
enum Command
  AddItem(text: Text)
  ListItems
  MarkDone(number: Int)
  RemoveItem(number: Int)
  ShowHelp
end

## Why a command couldn't be carried out. `main` prints the error's text form
## (see `to_text`) to standard error, and the program exits with status 1.
enum TodoError
  Usage(problem: Text)
  NotANumber(given: Text)
  NoSuchItem(number: Int, count: Int)
  FileProblem(FileError)
end

## Carries out the one command given on the command line. Only `add`, `done`
## and `remove` change todo.txt.
fn main(args: Args, console: Console, files: Files) : Result(Unit, TodoError)
  command = try parse_command(args.list)
  case command
    AddItem(text: text): add_item(text, console, files)
    ListItems: list_items(console, files.read_only)
    MarkDone(number: number): mark_done(number, console, files)
    RemoveItem(number: number): remove_item(number, console, files)
    ShowHelp:
      console.print(usage)
      Ok(Unit)
  end
end

## `todo add`: adds an item, still to do, to the end of the list.
fn add_item(text: Text, console: Console, files: Files) : Result(Unit, TodoError)
  items = try load(files.read_only)
  try save(files, items.append(Item(text: text, done?: false)))
  console.print("Added item #{items.size + 1}: #{text}")
  Ok(Unit)
end

## `todo list`: prints the list, numbered from 1.
fn list_items(console: Console, files: ReadFiles) : Result(Unit, TodoError)
  items = try load(files)
  if items.empty?
    console.print("Nothing to do: the list is empty.")
  end
  for line in numbered(items)
    console.print(line)
  end
  Ok(Unit)
end

## `todo done N`: marks item N as done. An item that is already done stays as
## it is.
fn mark_done(number: Int, console: Console, files: Files) : Result(Unit, TodoError)
  items = try load(files.read_only)
  item = try item_at(items, number)
  if item.done?
    console.print("Item #{number} was already done: #{item.text}")
    return Ok(Unit)
  end
  finished = Item(text: item.text, done?: true)
  changed = items.set(number - 1, finished) or
    stop "item_at found the item, so its position exists"
  try save(files, changed)
  console.print("Marked item #{number} as done: #{item.text}")
  Ok(Unit)
end

## `todo remove N`: removes item N. The items after it move up by one.
fn remove_item(number: Int, console: Console, files: Files) : Result(Unit, TodoError)
  items = try load(files.read_only)
  item = try item_at(items, number)
  rest = items.remove_at(number - 1) or
    stop "item_at found the item, so its position exists"
  try save(files, rest)
  console.print("Removed item #{number}: #{item.text}")
  Ok(Unit)
end

## The items in the list, in order. With no file yet, the list is empty.
fn load(files: ReadFiles) : Result(List(Item), TodoError)
  case files.read_text(todo_file)
    Ok(text): Ok(parse_items(text))
    Error(NotFound(path: _)): Ok([])
    Error(problem): Error(FileProblem(problem))
  end
end

## Replaces the file with these items, one per line.
fn save(files: Files, items: List(Item)) : Result(Unit, TodoError)
  try files.write_text(todo_file, file_text(items))
  Ok(Unit)
end

## The text of the file for these items: one line each.
fn file_text(items: List(Item)) : Text
  promise "reading the text back gives the same items, when their texts are as add makes them"
    items.all?(fn(each) item_text([each.text]) == each.text end) implies parse_items(result) == items
    allows items: [Item(text: "buy milk", done?: true)], result: "[x] buy milk\n"
    forbids items: [Item(text: "buy milk", done?: true)], result: "[ ] buy milk\n"
  end
  examples
    file_text([]) == ""
    file_text([Item(text: "buy milk", done?: false), Item(text: "call the bank", done?: true)]) ==
      "[ ] buy milk\n[x] call the bank\n"
  end

  items.map(fn(item) "#{format_item(item)}\n" end).join("")
end

## Reads the file's text: one item per line, skipping blank lines.
fn parse_items(text: Text) : List(Item)
  examples
    parse_items("") == []
    parse_items("[ ] buy milk\n\n[x] call the bank\r\n") ==
      [Item(text: "buy milk", done?: false), Item(text: "call the bank", done?: true)]
  end

  text.lines.filter(fn(line) not line.trim.empty? end).map(parse_item)
end

## Writes one item as a line of the file, without the line break.
fn format_item(item: Item) : Text
  examples
    format_item(Item(text: "buy milk", done?: false)) == "[ ] buy milk"
    format_item(Item(text: "call the bank", done?: true)) == "[x] call the bank"
  end

  mark = if item.done?: done_mark else: open_mark
  "#{mark} #{item.text}"
end

## Reads one line of the file. A line that starts with neither mark is an item
## still to do, so a list written by hand works too.
fn parse_item(line: Text) : Item
  examples
    parse_item("[ ] buy milk") == Item(text: "buy milk", done?: false)
    parse_item("[x] call the bank") == Item(text: "call the bank", done?: true)
    parse_item("  [X]call the bank ") == Item(text: "call the bank", done?: true)
    parse_item("water the plants") == Item(text: "water the plants", done?: false)
    parse_item("[ ] [x] means done") == Item(text: "[x] means done", done?: false)
  end

  trimmed = line.trim
  if trimmed.drop_prefix(done_mark) is Some(done_text)
    Item(text: done_text.trim, done?: true)
  else if trimmed.drop_prefix("[X]") is Some(capital_text)
    Item(text: capital_text.trim, done?: true)
  else if trimmed.drop_prefix(open_mark) is Some(open_text)
    Item(text: open_text.trim, done?: false)
  else
    Item(text: trimmed, done?: false)
  end
end

## The lines `todo list` prints: each item with its number, counting from 1.
## The numbers are aligned on the right, so the items line up.
fn numbered(items: List(Item)) : List(Text)
  promise "there is one line for each item, in order, ending with the item as the file holds it"
    result.size == items.size
    result.zip(items).all?(fn(entry) entry.0.ends_with?(format_item(entry.1)) end)
    allows items: [Item(text: "buy milk", done?: false)], result: ["1. [ ] buy milk"]
    forbids items: [Item(text: "buy milk", done?: false)], result: ["1. [x] buy milk"]
    forbids items: [], result: ["1. [ ] buy milk"]
  end
  examples
    numbered([]) == []
    numbered([Item(text: "buy milk", done?: false), Item(text: "call the bank", done?: true)]) ==
      ["1. [ ] buy milk", "2. [x] call the bank"]
  end

  width = "#{items.size}".char_count
  items.enumerate.map(fn(pair)
    (index, item) = pair
    label = "#{index + 1}.".pad_left(width + 1)
    "#{label} #{format_item(item)}"
  end)
end

## The item numbered `number`, counting from 1.
fn item_at(items: List(Item), number: Int) : Result(Item, TodoError)
  examples
    item_at([Item(text: "buy milk", done?: false)], 1) == Ok(Item(text: "buy milk", done?: false))
    item_at([Item(text: "buy milk", done?: false)], 2) == Error(NoSuchItem(number: 2, count: 1))
    item_at([], 0) == Error(NoSuchItem(number: 0, count: 0))
  end

  missing = NoSuchItem(number: number, count: items.size)
  return Error(missing) if number < 1
  items.get(number - 1).or_fail(missing)
end

## Reads the command line: the arguments after the program's name.
fn parse_command(words: List(Text)) : Result(Command, TodoError)
  examples
    parse_command(["add", "buy", "milk"]) == Ok(AddItem(text: "buy milk"))
    parse_command(["list"]) == Ok(ListItems)
    parse_command(["done", "2"]) == Ok(MarkDone(number: 2))
    parse_command(["remove", "3"]) == Ok(RemoveItem(number: 3))
    parse_command(["remove", "two"]) == Error(NotANumber(given: "two"))
    parse_command(["add", " "]) is Error(Usage(problem: _))
    parse_command(["done"]) is Error(Usage(problem: _))
    parse_command([]) is Error(Usage(problem: _))
  end

  case words
    ["add", ..rest]:
      text = item_text(rest)
      return Error(Usage(problem: "say what to add, as in: todo add buy milk")) if text.empty?
      Ok(AddItem(text: text))
    ["list"]: Ok(ListItems)
    ["done", given]: Ok(MarkDone(number: try parse_number(given)))
    ["remove", given]: Ok(RemoveItem(number: try parse_number(given)))
    ["help"] | ["--help"] | ["-h"]: Ok(ShowHelp)
    ["list", .._]: Error(Usage(problem: "list takes nothing after it"))
    ["done", .._]: Error(Usage(problem: "done takes one item number, as in: todo done 2"))
    ["remove", .._]: Error(Usage(problem: "remove takes one item number, as in: todo remove 2"))
    [other, .._]: Error(Usage(problem: "there is no command \"#{other}\""))
    []: Error(Usage(problem: "say what to do"))
  end
end

## The text of a new item, from the words after `add`: joined by spaces, kept
## to one line, as each item is one line of the file, and trimmed.
fn item_text(words: List(Text)) : Text
  examples
    item_text(["buy", "milk"]) == "buy milk"
    item_text([" call the bank\non Monday "]) == "call the bank on Monday"
    item_text([]) == ""
  end

  words.join(" ").replace("\r\n", " ").replace("\n", " ").replace("\r", " ").trim
end

## An item number, as given on the command line.
fn parse_number(given: Text) : Result(Int, TodoError)
  examples
    parse_number("3") == Ok(3)
    parse_number("three") == Error(NotANumber(given: "three"))
  end

  given.trim.to_int.or_fail(NotANumber(given: given))
end

## The text form of an error: what `main` prints to standard error.
fn to_text(failure: TodoError) : Text
  case failure
    Usage(problem: problem): "todo: #{problem}\n#{usage}"
    NotANumber(given: given): "todo: \"#{given}\" is not an item number; items are numbered from 1"
    NoSuchItem(number: number, count: 0): "todo: there is no item #{number}: the list is empty"
    NoSuchItem(number: number, count: 1): "todo: there is no item #{number}: the list has only item 1"
    NoSuchItem(number: number, count: count):
      "todo: there is no item #{number}: the items are numbered 1 to #{count}"
    FileProblem(problem): "todo: #{problem}"
  end
end

test "items are added, marked done, removed and listed"
  console = Console.fake()
  files = Files.in_memory({})
  assert main(Args.of(["add", "buy", "milk"]), console, files) == Ok(Unit)
  assert main(Args.of(["add", "call the bank"]), console, files) == Ok(Unit)
  assert main(Args.of(["add", "water the plants"]), console, files) == Ok(Unit)
  assert main(Args.of(["done", "2"]), console, files) == Ok(Unit)
  assert main(Args.of(["remove", "1"]), console, files) == Ok(Unit)
  assert main(Args.of(["list"]), console, files) == Ok(Unit)
  assert files.read_text("todo.txt") == Ok("[x] call the bank\n[ ] water the plants\n")
  assert console.printed == [
    "Added item 1: buy milk",
    "Added item 2: call the bank",
    "Added item 3: water the plants",
    "Marked item 2 as done: call the bank",
    "Removed item 1: buy milk",
    "1. [x] call the bank",
    "2. [ ] water the plants"
  ]
end

test "with no file, the list is empty and listing makes no file"
  console = Console.fake()
  files = Files.in_memory({})
  assert main(Args.of(["list"]), console, files) == Ok(Unit)
  assert console.printed == ["Nothing to do: the list is empty."]
  assert not files.exists?("todo.txt")
end

test "a list written by hand is read, and blank lines are skipped"
  console = Console.fake()
  files = Files.in_memory({"todo.txt": "buy milk\n\n  [X] call the bank\r\n"})
  assert main(Args.of(["list"]), console, files) == Ok(Unit)
  assert console.printed == ["1. [ ] buy milk", "2. [x] call the bank"]
end

test "marking an item done again leaves it done"
  console = Console.fake()
  files = Files.in_memory({"todo.txt": "[x] buy milk\n"})
  assert main(Args.of(["done", "1"]), console, files) == Ok(Unit)
  assert console.printed == ["Item 1 was already done: buy milk"]
  assert files.read_text("todo.txt") == Ok("[x] buy milk\n")
end

test "a wrong item number is reported, and the file is left as it was"
  console = Console.fake()
  files = Files.in_memory({"todo.txt": "[ ] buy milk\n"})
  assert main(Args.of(["done", "2"]), console, files) == Error(NoSuchItem(number: 2, count: 1))
  assert main(Args.of(["remove", "0"]), console, files) == Error(NoSuchItem(number: 0, count: 1))
  assert main(Args.of(["done", "first"]), console, files) == Error(NotANumber(given: "first"))
  assert main(Args.of(["remove"]), console, files) is Error(Usage(problem: _))
  assert files.read_text("todo.txt") == Ok("[ ] buy milk\n")
  assert console.printed == []
end

test "errors say what went wrong"
  assert "#{NoSuchItem(number: 3, count: 2)}" == "todo: there is no item 3: the items are numbered 1 to 2"
  assert "#{NoSuchItem(number: 1, count: 0)}" == "todo: there is no item 1: the list is empty"
  assert "#{NotANumber(given: "x")}" == "todo: \"x\" is not an item number; items are numbered from 1"
  assert "#{FileProblem(NotAllowed(path: "todo.txt"))}" == "todo: todo.txt: not allowed"
end

test "the numbers line up when the list has ten items or more"
  items = (1..11).to_list.map(fn(n) Item(text: "task #{n}", done?: false) end)
  lines = numbered(items)
  assert lines.first == Some(" 1. [ ] task 1")
  assert lines.last == Some("10. [ ] task 10")
end
