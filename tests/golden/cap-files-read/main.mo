module Main

fn first_line(files: ReadFiles, path: Text) : Text
  case files.read_text(path)
    Ok(text): text.lines.first or "(empty)"
    Error(failure): "error: #{failure}"
  end
end

fn main(console: Console, files: ReadFiles)
  dir = "tests/golden/cap-files-read/data"
  console.print(first_line(files, "#{dir}/list.txt"))
  console.print(first_line(files, "#{dir}/note.txt"))
  console.print(first_line(files, "#{dir}/missing.txt"))
  console.print(first_line(files, "../main.mo"))
  case files.list(dir)
    Ok(names): console.print(names.join(","))
    Error(failure): console.print("error: #{failure}")
  end
end
