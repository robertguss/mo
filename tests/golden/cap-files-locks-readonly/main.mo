module Main
use Api

enum StoreError
  Io(Text)
end

api
  post "/write" post_write
  get "/read" get_read
  status Io(_) = 500
end

fn map_io(res: Result(T, FileError)) : Result(T, StoreError)
  res.map_error(fn(e) Io(e.to_text) end)
end

fn post_write(files: Files) : Result(Text, StoreError)
  fh = try map_io(files.open("store.bin"))
  try map_io(fh.lock_exclusive)
  try map_io(fh.write_at(0, "hello-lock".bytes))
  try map_io(fh.sync)
  try map_io(fh.unlock)
  Ok("written")
end

fn get_read(files: ReadFiles) : Result(Text, StoreError)
  fh = try map_io(files.open("store.bin"))
  try map_io(fh.lock_shared)
  n = try map_io(fh.size)
  raw = try map_io(fh.read_at(0, n))
  try map_io(fh.unlock)
  Ok(raw.decode_utf8 or "bad")
end

fn hold_exclusive_and_drop(files: Files, path: Text) : Result(Unit, FileError)
  fh = try files.open(path)
  try fh.lock_exclusive
  Ok(Unit)
end

test "advisory file locks upgrade downgrade conflict and auto-release on drop"
  files = Files.in_memory({"a.bin": "seed"})
  assert files.open("a.bin") is Ok(h1)
  assert files.open("a.bin") is Ok(h2)
  assert h1.unlock == Ok(Unit)
  assert h1.lock_shared == Ok(Unit)
  assert h2.lock_shared == Ok(Unit)
  assert h1.lock_exclusive == Error(Failed(path: "a.bin", reason: "file is locked"))
  assert h2.unlock == Ok(Unit)
  assert h1.lock_exclusive == Ok(Unit)
  assert h1.lock_exclusive == Ok(Unit)
  assert h2.lock_shared == Error(Failed(path: "a.bin", reason: "file is locked exclusively"))
  assert h2.lock_exclusive == Error(Failed(path: "a.bin", reason: "file is locked"))
  assert h1.lock_shared == Ok(Unit)
  assert h2.lock_shared == Ok(Unit)
  assert h1.unlock == Ok(Unit)
  assert h2.unlock == Ok(Unit)
  assert hold_exclusive_and_drop(files, "b.bin") == Ok(Unit)
  assert files.open("b.bin") is Ok(h3)
  assert h3.lock_exclusive == Ok(Unit)
  assert h3.unlock == Ok(Unit)
end

test "ReadFiles.open enforces confinement and read-only attenuation"
  files = Files.in_memory({"dir/data.bin": "012345"})
  ro = files.read_only
  assert ro.open("missing.bin") is Error(NotFound(path: "missing.bin"))
  assert not ro.exists?("missing.bin")
  assert ro.open("../escape.bin") is Error(NotAllowed(path: "../escape.bin"))
  assert ro.open("dir") is Error(Failed(path: "dir", reason: "is a folder"))
  assert ro.open("dir/data.bin") is Ok(fh)
  assert fh.size == Ok(6)
  assert fh.read_at(1, 3) == Ok("123".bytes)
  assert fh.lock_shared == Ok(Unit)
  assert fh.unlock == Ok(Unit)
  assert fh.write_at(0, "x".bytes) == Error(NotAllowed(path: "dir/data.bin"))
  assert fh.truncate(2) == Error(NotAllowed(path: "dir/data.bin"))
  assert fh.sync == Error(NotAllowed(path: "dir/data.bin"))
  assert fh.lock_exclusive == Error(NotAllowed(path: "dir/data.bin"))
  assert files.read_text("dir/data.bin") == Ok("012345")
end

test "ApiTest.send derives ReadFiles from supplied Files for read routes"
  files = Files.in_memory({})
  assert ApiTest.send(Main, Api.post("/write"), files).text == "\"written\""
  assert ApiTest.send(Main, Api.get("/read"), files).text == "\"hello-lock\""
end
