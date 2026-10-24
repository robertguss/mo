## A to-do list kept in the file todo.txt in the current folder.
module Todo

struct Item
  done?: Bool
  text: Text
end

usage = "usage: todo add TEXT... | todo list | todo done N | todo edit N TEXT... | todo clear"

## Reads one line of todo.txt: `[ ] TEXT` or `[x] TEXT`.
fn parse_item(line: Text) : Option(Item)
  examples
    parse_item("[ ] buy milk") == Some(Item(done?: false, text: "buy milk"))
    parse_item("[x] call Bo") == Some(Item(done?: true, text: "call Bo"))
    parse_item("buy milk") == None
  end

  case line.drop_prefix("[ ] ")
    Some(text): Some(Item(done?: false, text: text))
    None: line.drop_prefix("[x] ").map(fn(text) Item(done?: true, text: text) end)
  end
end

fn format_item(item: Item) : Text
  "#{if item.done?: "[x]" else: "[ ]"} #{item.text}"
end

fn load(files: Files) : Result(List(Item), Text)
  return Ok([]) if not files.exists?("todo.txt")
  text = try files.read_text("todo.txt").map_error(fn(failure) "#{failure}" end)
  var items = []
  for (index, line) in text.lines.enumerate
    case parse_item(line)
      Some(item): items = items.append(item)
      None: return Error("todo.txt:#{index + 1}: not an item: #{line}")
    end
  end
  Ok(items)
end

fn save(files: Files, items: List(Item)) : Result(Unit, Text)
  text = items.map(fn(item) "#{format_item(item)}\n" end).join("")
  files.write_text("todo.txt", text).map_error(fn(failure) "#{failure}" end)
end

## The position, from 1, that `text` names in a list of `size` items.
fn position(text: Text, size: Int) : Option(Int)
  examples
    position("2", 3) == Some(2)
    position("4", 3) == None
    position("0", 3) == None
    position("x", 3) == None
  end

  n = try text.to_int
  if n >= 1 and n <= size: Some(n) else: None
end

## Runs one command, returning what to print, or why it failed.
fn run(command: List(Text), files: Files) : Result(List(Text), Text)
  items = try load(files)
  case command
    ["add", ..words] if not words.empty?:
      text = words.join(" ")
      try save(files, items.append(Item(done?: false, text: text)))
      Ok(["added #{items.size + 1}: #{text}"])
    ["list"]: Ok(items.enumerate.map(fn(pair) "#{pair.0 + 1}. #{format_item(pair.1)}" end))
    ["done", number]:
      n = try position(number, items.size).or_fail("no item #{number}")
      item = items.get(n - 1) or stop "position checked n against the list"
      changed = try items.set(n - 1, Item(done?: true, text: item.text)).or_fail(
        "no item #{number}"
      )
      try save(files, changed)
      Ok(["done #{n}: #{item.text}"])
    ["edit", number, ..words] if not words.empty?:
      n = try position(number, items.size).or_fail("no item #{number}")
      item = items.get(n - 1) or stop "position checked n against the list"
      text = words.join(" ")
      changed = try items.set(n - 1, Item(done?: item.done?, text: text)).or_fail(
        "no item #{number}"
      )
      try save(files, changed)
      Ok(["edited #{n}: #{text}"])
    ["clear"]:
      remaining = items.filter(fn(item) not item.done? end)
      count = items.size - remaining.size
      try save(files, remaining)
      Ok(["cleared #{count} done #{if count == 1: "item" else: "items"}"])
    _: Error(usage)
  end
end

fn main(args: Args, console: Console, files: Files) : Int
  case run(args.list, files)
    Ok(lines):
      for line in lines
        console.print(line)
      end
      0
    Error(message):
      console.error(message)
      1
  end
end

test "items are added, listed and done"
  files = Files.in_memory({})
  assert run(["add", "buy", "milk"], files) == Ok(["added 1: buy milk"])
  assert run(["add", "call Bo"], files) == Ok(["added 2: call Bo"])
  assert run(["done", "1"], files) == Ok(["done 1: buy milk"])
  assert run(["list"], files) == Ok(["1. [x] buy milk", "2. [ ] call Bo"])
  assert files.read_text("todo.txt") == Ok("[x] buy milk\n[ ] call Bo\n")
end

test "editing joins words and keeps each item's done state"
  files = Files.in_memory({"todo.txt": "[ ] buy milk\n[x] call Bo\n[ ] keep me\n"})
  console = Console.fake()
  assert main(Args.of(["edit", "1", "buy", "oat", "milk"]), console, files) == 0
  assert console.printed == ["edited 1: buy oat milk"]
  assert console.printed_errors == []
  assert run(["edit", "2", "call", "Ada"], files) == Ok(["edited 2: call Ada"])
  assert files.read_text("todo.txt") == Ok("[ ] buy oat milk\n[x] call Ada\n[ ] keep me\n")
end

test "clearing removes done items and renumbers the rest in order"
  files = Files.in_memory({"todo.txt": "[x] first\n[ ] second\n[x] third\n[ ] fourth\n"})
  console = Console.fake()
  assert main(Args.of(["clear"]), console, files) == 0
  assert console.printed == ["cleared 2 done items"]
  assert console.printed_errors == []
  assert run(["list"], files) == Ok(["1. [ ] second", "2. [ ] fourth"])
  assert files.read_text("todo.txt") == Ok("[ ] second\n[ ] fourth\n")
  assert run(["clear"], files) == Ok(["cleared 0 done items"])
  assert files.read_text("todo.txt") == Ok("[ ] second\n[ ] fourth\n")
end

test "clearing the last done item keeps an empty todo file"
  files = Files.in_memory({"todo.txt": "[x] finished\n"})
  assert run(["clear"], files) == Ok(["cleared 1 done item"])
  assert files.exists?("todo.txt")
  assert files.read_text("todo.txt") == Ok("")
  assert run(["list"], files) == Ok([])
  assert run(["clear"], files) == Ok(["cleared 0 done items"])
  assert files.read_text("todo.txt") == Ok("")
end

test "invalid edit positions fail on stderr without changing the file"
  original = "[ ] first\n[x] second\n[ ] third\n"
  files = Files.in_memory({"todo.txt": original})
  for number in ["-1", "0", "4", "x", "9223372036854775808"]
    console = Console.fake()
    assert main(Args.of(["edit", number, "replacement"]), console, files) == 1
    assert console.printed == []
    assert console.printed_errors == ["no item #{number}"]
    assert files.read_text("todo.txt") == Ok(original)
  end
end

test "invalid command lines print usage and leave the file unchanged"
  original = "[ ] first\n[x] second\n"
  files = Files.in_memory({"todo.txt": original})
  for command in [[], ["edit"], ["edit", "2"], ["clear", "all"], ["unknown"]]
    console = Console.fake()
    assert main(Args.of(command), console, files) == 1
    assert console.printed == []
    assert console.printed_errors == [
      "usage: todo add TEXT... | todo list | todo done N | todo edit N TEXT... | todo clear"
    ]
    assert files.read_text("todo.txt") == Ok(original)
  end
end

test "edit and clear handle a missing todo file"
  files = Files.in_memory({})
  assert run(["edit", "1", "replacement"], files) == Error("no item 1")
  assert not files.exists?("todo.txt")
  assert run(["clear"], files) == Ok(["cleared 0 done items"])
  assert files.read_text("todo.txt") == Ok("")
end
