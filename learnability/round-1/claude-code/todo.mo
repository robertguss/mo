## todo: a to-do list kept in the file todo.txt, in the folder the program
## is run in.
##
##   todo add TEXT    adds TEXT to the end of the list
##   todo list        shows the list, numbered from 1
##   todo done N      marks item N as done
##   todo remove N    removes item N from the list
##
## Each line of todo.txt is one item: "[ ] " and its text while it is still to
## do, or "[x] " and its text once it is done. The file can be edited by hand:
## a line without a mark is an item still to do, and blank lines are skipped.
module Todo

todo_file = "todo.txt"

usage = """
  Usage:
    todo add TEXT    add TEXT to the end of the list
    todo list        show the list, numbered from 1
    todo done N      mark item N as done
    todo remove N    remove item N from the list
  """

## One item on the list.
struct Item
  text: Text
  done?: Bool
end

## What can go wrong. `describe` puts each one in words.
enum TodoError
  BadUsage
  NothingToAdd
  NotANumber(text: Text)
  NoSuchItem(number: Int)
  FileTrouble(FileError)
end

## Runs one command. The exit status is 0 when it succeeds, 1 when it fails,
## and 2 when the arguments aren't a command, after printing how to use todo.
fn main(args: Args, console: Console, files: Files) : Int
  case run(args.list, console, files)
    Ok(_): 0
    Error(BadUsage):
      console.error(usage)
      2
    Error(failure):
      console.error("todo: #{describe(failure)}")
      1
  end
end

## Runs the command that the program's arguments name.
fn run(words: List(Text), console: Console, files: Files) : Result(Unit, TodoError)
  case words
    ["add", ..parts]: add_item(parts, console, files)
    ["list"]: list_items(console, files.read_only)
    ["done", number]: mark_done(number, console, files)
    ["remove", number]: remove_item(number, console, files)
    ["help"] | ["--help"]:
      console.print(usage)
      Ok(Unit)
    _: Error(BadUsage)
  end
end

## Adds the words after "add", joined by spaces, to the end of the list.
fn add_item(words: List(Text), console: Console, files: Files) : Result(Unit, TodoError)
  text = words.join(" ").replace("\r", " ").replace("\n", " ").trim
  return Error(NothingToAdd) if text.empty?
  items = try load(files.read_only)
  try save(files, items.append(Item(text: text, done?: false)))
  console.print("Added #{items.size + 1}: #{text}")
  Ok(Unit)
end

## Prints every item, numbered from 1, with "[x]" on those that are done.
fn list_items(console: Console, files: ReadFiles) : Result(Unit, TodoError)
  items = try load(files)
  if items.empty?
    console.print("The list is empty.")
    return Ok(Unit)
  end
  width = items.size.to_text.char_count
  for (index, item) in items.enumerate
    label = (index + 1).to_text.pad_left(width)
    console.print("#{label}. #{format_item(item)}")
  end
  Ok(Unit)
end

## Marks the item numbered `number_text` as done.
fn mark_done(number_text: Text, console: Console, files: Files) : Result(Unit, TodoError)
  number = try item_number(number_text)
  items = try load(files.read_only)
  item = try items.get(number - 1).or_fail(NoSuchItem(number: number))
  if item.done?
    console.print("Item #{number} was already done: #{item.text}")
    return Ok(Unit)
  end
  marked = Item(text: item.text, done?: true)
  updated = try items.set(number - 1, marked).or_fail(NoSuchItem(number: number))
  try save(files, updated)
  console.print("Marked #{number} as done: #{item.text}")
  Ok(Unit)
end

## Removes the item numbered `number_text`; the items after it move up one.
fn remove_item(number_text: Text, console: Console, files: Files) : Result(Unit, TodoError)
  number = try item_number(number_text)
  items = try load(files.read_only)
  item = try items.get(number - 1).or_fail(NoSuchItem(number: number))
  remaining = try items.remove_at(number - 1).or_fail(NoSuchItem(number: number))
  try save(files, remaining)
  console.print("Removed #{number}: #{item.text}")
  Ok(Unit)
end

## Reads an item number as the list shows it, counting from 1.
fn item_number(text: Text) : Result(Int, TodoError)
  promise "a number it accepts counts from 1"
    result is Ok(number) implies number >= 1
    allows text: "3", result: Ok(3)
    forbids text: "0", result: Ok(0)
  end
  examples
    item_number("2") == Ok(2)
    item_number("0") == Error(NoSuchItem(number: 0))
    item_number("two") == Error(NotANumber(text: "two"))
  end

  case text.trim.to_int
    Some(n) if n >= 1: Ok(n)
    Some(n): Error(NoSuchItem(number: n))
    None: Error(NotANumber(text: text))
  end
end

## The items in todo.txt, in order; none when the file doesn't exist yet.
fn load(files: ReadFiles) : Result(List(Item), TodoError)
  case files.read_text(todo_file)
    Ok(text): Ok(text.lines.filter_map(parse_line))
    Error(NotFound(path: _)): Ok([])
    Error(problem): Error(FileTrouble(problem))
  end
end

## Writes the items to todo.txt, one per line, replacing what it held.
fn save(files: Files, items: List(Item)) : Result(Unit, TodoError)
  text = items.map(fn(item) "#{format_item(item)}\n" end).join("")
  try files.write_text(todo_file, text)
  Ok(Unit)
end

## Reads one line of todo.txt; a blank line holds no item.
fn parse_line(line: Text) : Option(Item)
  examples
    parse_line("[x] buy milk") == Some(Item(text: "buy milk", done?: true))
    parse_line("[ ] buy milk") == Some(Item(text: "buy milk", done?: false))
    parse_line("buy milk") == Some(Item(text: "buy milk", done?: false))
    parse_line("  ") == None
  end

  trimmed = line.trim
  return None if trimmed.empty?
  case trimmed.drop_prefix("[x]")
    Some(rest): Some(Item(text: rest.trim, done?: true))
    None:
      text = trimmed.drop_prefix("[ ]") or trimmed
      Some(Item(text: text.trim, done?: false))
  end
end

## An item as a line of todo.txt, which is also how the list shows it.
fn format_item(item: Item) : Text
  promise "an item with no space around its text reads back as itself"
    item.text == item.text.trim implies parse_line(result) == Some(item)
    allows item: Item(text: "milk", done?: true), result: "[x] milk"
    forbids item: Item(text: "milk", done?: true), result: "[ ] milk"
  end
  examples
    format_item(Item(text: "buy milk", done?: false)) == "[ ] buy milk"
    format_item(Item(text: "buy milk", done?: true)) == "[x] buy milk"
  end

  box = if item.done?: "[x]" else: "[ ]"
  "#{box} #{item.text}"
end

## What went wrong, in words.
fn describe(failure: TodoError) : Text
  case failure
    BadUsage: "expected add, list, done or remove; see \"todo help\""
    NothingToAdd: "nothing to add: write the item after \"todo add\""
    NotANumber(text: given): "\"#{given}\" is not an item number"
    NoSuchItem(number: n): "there is no item #{n}"
    FileTrouble(problem): "#{problem}"
  end
end

test "added items are listed, numbered from 1"
  console = Console.fake()
  files = Files.in_memory({})
  assert main(Args.of(["add", "buy", "milk"]), console, files) == 0
  assert main(Args.of(["add", "walk the dog"]), console, files) == 0
  assert main(Args.of(["list"]), console, files) == 0
  assert console.printed == [
    "Added 1: buy milk",
    "Added 2: walk the dog",
    "1. [ ] buy milk",
    "2. [ ] walk the dog"
  ]
  assert files.read_text("todo.txt") == Ok("[ ] buy milk\n[ ] walk the dog\n")
end

test "done marks an item, and remove takes one out"
  console = Console.fake()
  files = Files.in_memory({"todo.txt": "[ ] buy milk\n[ ] walk the dog\n"})
  assert main(Args.of(["done", "2"]), console, files) == 0
  assert main(Args.of(["done", "2"]), console, files) == 0
  assert main(Args.of(["remove", "1"]), console, files) == 0
  assert main(Args.of(["list"]), console, files) == 0
  assert console.printed == [
    "Marked 2 as done: walk the dog",
    "Item 2 was already done: walk the dog",
    "Removed 1: buy milk",
    "1. [x] walk the dog"
  ]
  assert files.read_text("todo.txt") == Ok("[x] walk the dog\n")
end

test "a missing item, a bad number or no text is refused, and the file is kept"
  console = Console.fake()
  files = Files.in_memory({"todo.txt": "[ ] buy milk\n"})
  assert main(Args.of(["done", "2"]), console, files) == 1
  assert main(Args.of(["remove", "first"]), console, files) == 1
  assert main(Args.of(["add", " "]), console, files) == 1
  assert console.printed_errors == [
    "todo: there is no item 2",
    "todo: \"first\" is not an item number",
    "todo: nothing to add: write the item after \"todo add\""
  ]
  assert files.read_text("todo.txt") == Ok("[ ] buy milk\n")
end

test "an empty list says so"
  console = Console.fake()
  assert main(Args.of(["list"]), console, Files.in_memory({})) == 0
  assert console.printed == ["The list is empty."]
end

test "a file edited by hand is read line by line"
  console = Console.fake()
  files = Files.in_memory({"todo.txt": "call mum\n\n[x] pay rent\r\n"})
  assert main(Args.of(["list"]), console, files) == 0
  assert console.printed == ["1. [ ] call mum", "2. [x] pay rent"]
end

test "numbers line up once the list reaches 10 items"
  console = Console.fake()
  files = Files.in_memory({"todo.txt": "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\n"})
  assert main(Args.of(["list"]), console, files) == 0
  assert console.printed.first == Some(" 1. [ ] a")
  assert console.printed.last == Some("10. [ ] j")
end

test "anything else prints how to use todo, and changes nothing"
  console = Console.fake()
  files = Files.in_memory({})
  assert main(Args.of(["tidy"]), console, files) == 2
  assert main(Args.of([]), console, files) == 2
  assert console.printed.empty?
  assert not files.exists?("todo.txt")
end
