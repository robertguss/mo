module Main

fn show(found: Result(List(Entry), FileError)) : Text
  case found
    Ok(entries): entries.map(fn(e) "#{e.name}:#{kind(e.kind)}" end).join(" ")
    Error(failure): "error: #{failure}"
  end
end

fn kind(k: EntryKind) : Text
  case k
    File: "file"
    Folder: "folder"
    Link: "link"
    Other: "other"
  end
end

fn main(console: Console, files: ReadFiles)
  dir = "tests/golden/cap-files-walk/data"
  console.print(show(files.entries(dir)))
  console.print(show(files.walk(dir)))
  console.print(show(files.walk("#{dir}/missing")))
  console.print(show(files.walk("#{dir}/readme.txt")))
  console.print(
    "#{files.exists?("#{dir}/docs/deep/note.txt")} #{files.exists?("#{dir}/nope")} #{files.exists?(
      "../x"
    )}"
  )
  data = files.within(dir)
  console.print(show(data.walk(".")))
  case data.read_bytes("readme.txt")
    Ok(bytes): console.print("#{bytes == "top\n".bytes}")
    Error(failure): console.print("error: #{failure}")
  end
end
