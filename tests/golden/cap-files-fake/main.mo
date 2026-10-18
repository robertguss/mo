module Main

fn save(files: Files, name: Text, text: Text) : Result(Unit, FileError)
  files.write_text(name, text)
end

test "a fake keeps what is written to it"
  files = Files.in_memory({"a.txt": "one"})
  assert save(files, "b.txt", "two") == Ok(Unit)
  assert files.read_text("b.txt") == Ok("two")
  assert files.read_text("a.txt") == Ok("one")
  assert files.list(".") == Ok(["a.txt", "b.txt"])
end

test "a write replaces the whole file and a missing folder is refused"
  files = Files.in_memory({"a.txt": "one"})
  assert save(files, "a.txt", "uno") == Ok(Unit)
  assert files.read_text("a.txt") == Ok("uno")
  assert files.read_text("c.txt") == Error(NotFound(path: "c.txt"))
  assert save(files, "nope/x.txt", "y") == Error(NotFound(path: "nope/x.txt"))
end

test "read-only and paths that leave the folder"
  files = Files.in_memory({"a.txt": "one"}).read_only
  assert files.read_text("a.txt") == Ok("one")
  assert files.read_text("../a.txt") == Error(NotAllowed(path: "../a.txt"))
end
