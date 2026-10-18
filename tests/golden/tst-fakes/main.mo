module Main

fn main(args: Args, console: Console, files: ReadFiles) : Result(Unit, FileError)
  for name in args.list
    text = try files.read_text(name)
    console.print(text.lines.first or "")
  end
  Ok(Unit)
end

test "main prints the first line of each file"
  console = Console.fake()
  files = Files.in_memory({"a.txt": "one\ntwo\n", "b.txt": "three\n"}).read_only
  assert main(Args.of(["a.txt", "b.txt"]), console, files) == Ok(Unit)
  assert console.printed == ["one", "three"]
  assert console.printed_errors == []
end

test "a missing file is a failure and prints nothing more"
  console = Console.fake()
  files = Files.in_memory({"a.txt": "one\n"}).read_only
  assert main(Args.of(["a.txt", "gone.txt"]), console, files) == Error(NotFound(path: "gone.txt"))
  assert console.printed == ["one"]
end

test "fake input is read a line at a time"
  console = Console.fake_input(["x", "y"])
  assert console.read_line == Some("x")
  assert console.read_line == Some("y")
  assert console.read_line == None
end
