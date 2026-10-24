module Main

fn names(files: Files) : Result(List(Text), FileError)
  files.list(".")
end

test "plain relative paths make a fake"
  assert names(Files.in_memory({"a/b": "y", "c": "x"})) == Ok(["a", "c"])
end

test "a path that is both a file and a folder"
  assert names(Files.in_memory({"a": "x", "a/b": "y"})) == Ok(["a"])
end

test "an absolute path"
  assert names(Files.in_memory({"/a": "x"})) == Ok(["a"])
end

test "a path starting with a dot folder"
  assert names(Files.in_memory({"./a": "x"})) == Ok(["a"])
end

test "a path ending with a slash"
  assert names(Files.in_memory({"a/": "x"})) == Ok(["a"])
end

test "a path with an empty part"
  assert names(Files.in_memory({"a//b": "x"})) == Ok(["a"])
end
