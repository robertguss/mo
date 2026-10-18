module Main

test "make_folder makes missing folders above it and succeeds again"
  files = Files.in_memory({"a.txt": "one"})
  assert files.make_folder("x/y/z") == Ok(Unit)
  assert files.make_folder("x/y") == Ok(Unit)
  assert files.exists?("x") and files.exists?("x/y/z")
  assert files.list("x") == Ok(["y"])
  assert files.make_folder("a.txt") == Error(Failed(path: "a.txt", reason: "not a folder"))
  assert files.make_folder("../out") == Error(NotAllowed(path: "../out"))
end

test "entries give each name with its kind, in list's order"
  files = Files.in_memory({"b.txt": "", "a/c.txt": "", "a/d/e.txt": ""})
  assert files.entries(".") ==
    Ok([Entry(name: "a", kind: Folder), Entry(name: "b.txt", kind: File)])
  assert files.entries("a") ==
    Ok([Entry(name: "c.txt", kind: File), Entry(name: "d", kind: Folder)])
  assert files.entries("gone") == Error(NotFound(path: "gone"))
  assert files.entries("b.txt") == Error(Failed(path: "b.txt", reason: "not a folder"))
  case files.entries("a")
    Ok(found): assert found.map(fn(e) e.name end) == ["c.txt", "d"]
    Error(_): assert false
  end
end

test "exists? is true for files and folders and false outside"
  files = Files.in_memory({"a/b.txt": ""}).read_only
  assert files.exists?("a") and files.exists?("a/b.txt") and files.exists?(".")
  assert not files.exists?("c") and not files.exists?("a/b.txt/c")
  assert not files.exists?("../a")
end

test "walk names every entry by its path, each folder before what's inside it"
  files = Files.in_memory({"z.txt": "", "m/n.txt": "", "m/o/p.txt": ""})
  assert files.make_folder("m/empty") == Ok(Unit)
  assert files.walk(".") ==
    Ok([
      Entry(name: "m", kind: Folder),
      Entry(name: "m/empty", kind: Folder),
      Entry(name: "m/n.txt", kind: File),
      Entry(name: "m/o", kind: Folder),
      Entry(name: "m/o/p.txt", kind: File),
      Entry(name: "z.txt", kind: File)
    ])
  assert files.read_only.walk("m/o") == Ok([Entry(name: "p.txt", kind: File)])
  assert files.walk("z.txt") == Error(Failed(path: "z.txt", reason: "not a folder"))
  assert files.walk("../m") == Error(NotAllowed(path: "../m"))
end

test "append_text, remove, read_bytes and within"
  files = Files.in_memory({"log.txt": "a"})
  assert files.append_text("log.txt", "b") == Ok(Unit)
  assert files.append_text("new.txt", "c") == Ok(Unit)
  assert files.read_text("log.txt") == Ok("ab")
  assert files.read_text("new.txt") == Ok("c")
  assert files.read_bytes("log.txt") == Ok("ab".bytes)
  assert files.remove("new.txt") == Ok(Unit)
  assert files.remove("new.txt") == Error(NotFound(path: "new.txt"))
  assert files.make_folder("sub") == Ok(Unit)
  inner = files.within("sub")
  assert inner.write_text("in.txt", "x") == Ok(Unit)
  assert files.read_text("sub/in.txt") == Ok("x")
  assert files.read_only.within("sub").read_bytes("in.txt") == Ok("x".bytes)
  assert inner.read_text("../log.txt") == Error(NotAllowed(path: "../log.txt"))
end
