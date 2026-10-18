module Main

fn main(console: Console, files: ReadFiles) : Result(Unit, FileError)
  console.print("start")
  text = try files.read_text("tests/golden/fail-main-result-error/absent.txt")
  console.print(text)
  Ok(Unit)
end
