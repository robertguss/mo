## A persistent to-do list in todo.txt, with one task per line.
module Todo

struct Task
  text: Text
  done?: Bool
end

enum TodoError
  IoFailure(FileError)
  Usage
  InvalidTaskText
  InvalidTaskNumber(text: Text)
  MissingTask(number: Int)
end

fn to_text(problem: TodoError) : Text
  case problem
    IoFailure(cause): cause.to_text
    Usage: "Usage: todo add TEXT | todo list | todo done N | todo remove N"
    InvalidTaskText: "Task text must be nonempty and must not contain line breaks."
    InvalidTaskNumber(text: text): "Invalid task number: #{text}. Use a positive integer."
    MissingTask(number: number): "No task numbered #{number}."
  end
end

fn load_tasks(files: ReadFiles) : Result(List(Task), TodoError)
  content = case files.read_text("todo.txt")
    Ok(text): text
    Error(NotFound(path: _)): ""
    Error(problem): return Error(IoFailure(problem))
  end
  var tasks = []
  for line in content.lines
    task = case line.drop_prefix("[x] ")
      Some(text): Task(text: text, done?: true)
      None: Task(text: line.drop_prefix("[ ] ") or line, done?: false)
    end
    tasks = tasks.append(task)
  end
  Ok(tasks)
end

fn save_tasks(files: Files, tasks: List(Task)) : Result(Unit, TodoError)
  lines = tasks.map(fn(task)
    marker = if task.done?: "[x]" else: "[ ]"
    "#{marker} #{task.text}\n"
  end)
  try files.write_text("todo.txt", lines.join(""))
  Ok(Unit)
end

fn update_task(files: Files, number_text: Text, remove?: Bool) : Result(Unit, TodoError)
  number = try number_text.to_int.or_fail(InvalidTaskNumber(text: number_text))
  return Error(InvalidTaskNumber(text: number_text)) if number <= 0
  tasks = try load_tasks(files.read_only)
  position = number - 1
  task = try tasks.get(position).or_fail(MissingTask(number: number))
  updated = if remove?
    try tasks.remove_at(position).or_fail(MissingTask(number: number))
  else
    completed = Task(text: task.text, done?: true)
    try tasks.set(position, completed).or_fail(MissingTask(number: number))
  end
  save_tasks(files, updated)
end

fn main(args: Args, console: Console, files: Files) : Result(Unit, TodoError)
  case args.list
    ["add", ..words]:
      text = words.join(" ")
      if text.trim.empty? or text.contains?("\n") or text.contains?("\r")
        return Error(InvalidTaskText)
      end
      tasks = try load_tasks(files.read_only)
      try save_tasks(files, tasks.append(Task(text: text, done?: false)))
    ["list"]:
      tasks = try load_tasks(files.read_only)
      for (index, task) in tasks.enumerate
        marker = if task.done?: "[x]" else: "[ ]"
        console.print("#{index + 1}. #{marker} #{task.text}")
      end
    ["done", number_text]:
      try update_task(files, number_text, false)
    ["remove", number_text]:
      try update_task(files, number_text, true)
    _: return Error(Usage)
  end
  Ok(Unit)
end
