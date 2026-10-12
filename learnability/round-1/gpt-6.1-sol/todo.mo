## A persistent to-do list. Storage lines are [ ] TEXT or [x] TEXT.
module Todo

struct Item
  text: Text
  done?: Bool
end

enum TodoError
  Storage(FileError)
  Usage(Text)
  BadFile(line: Int)
end

fn to_text(problem: TodoError) : Text
  case problem
    Storage(error): "#{error}"
    Usage(reason): reason
    BadFile(line: n): "todo.txt:#{n}: invalid stored item"
  end
end

fn load(files: ReadFiles) : Result(List(Item), TodoError)
  text = case files.read_text("todo.txt")
    Ok(value): value
    Error(NotFound(path: _)): return Ok([])
    Error(problem): return Error(Storage(problem))
  end
  var items = []
  for (index, line) in text.lines.enumerate
    if line.drop_prefix("[ ] ") is Some(body)
      items = items.append(Item(text: body, done?: false))
    else if line.drop_prefix("[x] ") is Some(body)
      items = items.append(Item(text: body, done?: true))
    else
      return Error(BadFile(line: index + 1))
    end
  end
  Ok(items)
end

fn save(items: List(Item), files: Files) : Result(Unit, TodoError)
  rows = items.map(fn(item)
    mark = if item.done?: "x" else: " "
    "[#{mark}] #{item.text}\n"
  end)
  try files.write_text("todo.txt", rows.join(""))
  Ok(Unit)
end

fn index(number: Text, count: Int) : Result(Int, TodoError)
  parsed = try number.to_int.or_fail(Usage("N must be a positive item number"))
  return Error(Usage("no item numbered #{parsed}")) if parsed < 1 or parsed > count
  Ok(parsed - 1)
end

fn main(args: Args, console: Console, files: Files) : Result(Unit, TodoError)
  items = try load(files.read_only)
  case args.list
    ["list"]:
      for (position, item) in items.enumerate
        mark = if item.done?: "x" else: " "
        console.print("#{position + 1}. [#{mark}] #{item.text}")
      end
      Ok(Unit)
    ["add", ..words]:
      text = words.join(" ").trim
      return Error(Usage("todo add needs nonempty single-line TEXT")) if text.empty? or
        text.contains?("\n") or text.contains?("\r")
      try save(items.append(Item(text: text, done?: false)), files)
      Ok(Unit)
    ["done", number]:
      position = try index(number, items.size)
      var item = try items.get(position).or_fail(Usage("no such item"))
      item.done? = true
      updated = try items.set(position, item).or_fail(Usage("no such item"))
      save(updated, files)
    ["remove", number]:
      position = try index(number, items.size)
      updated = try items.remove_at(position).or_fail(Usage("no such item"))
      save(updated, files)
    _: Error(Usage("Usage: todo add TEXT | list | done N | remove N"))
  end
end
