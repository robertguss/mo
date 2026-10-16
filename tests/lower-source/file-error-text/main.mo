module Main

fn render(value: T) : Text where T: Text
  "#{value}"
end

fn main(console: Console) : Int
  missing = NotFound(path: "notes/a.md")
  blocked = NotAllowed(path: "../secret")
  binary = NotText(path: "bin/app")
  failed = Failed(path: "out.txt", reason: "is a folder")
  reordered = Failed(reason: "is a folder", path: "out.txt")
  nested = Some(missing)
  console.print("#{missing}")
  console.print("#{blocked}")
  console.print("#{binary}")
  console.print("#{failed}")
  console.print(render(nested))
  if missing.to_text == "notes/a.md: no such file" and blocked.to_text == "../secret: not allowed" and binary.to_text == "bin/app: not text" and failed.to_text == "out.txt: is a folder" and reordered.to_text == "out.txt: is a folder" and render(failed) == "out.txt: is a folder" and nested.to_text == "Some(notes/a.md: no such file)": 0 else: 1
end
