module Main

fn save(files: ReadFiles) : Result(Unit, FileError)
  files.write_text("notes.txt", "hello")
end
