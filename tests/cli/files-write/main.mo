## Writes into the folder it is started in; ci/check_cli.py runs it in a fresh temporary folder.
module Main

fn main(console: Console, files: Files) : Result(Unit, FileError)
  try files.make_folder("out/logs/old")
  try files.make_folder("out/logs")
  try files.write_text("out/a.txt", "one\n")
  try files.append_text("out/a.txt", "two\n")
  try files.append_text("out/logs/run.txt", "start\n")
  try files.write_text("out/gone.txt", "x")
  try files.remove("out/gone.txt")
  logs = files.within("out/logs")
  try logs.write_text("old/kept.txt", "kept\n")
  console.print("#{files.exists?("out/logs/old")} #{files.exists?("out/gone.txt")}")
  for entry in try files.entries("out")
    console.print("entry #{entry.name}")
  end
  for entry in try files.walk("out")
    console.print("walk #{entry.name}")
  end
  console.print("#{files.make_folder("out/a.txt")}")
  Ok(Unit)
end
