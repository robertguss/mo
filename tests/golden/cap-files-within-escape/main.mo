module Main

fn report(console: Console, name: Text, files: ReadFiles)
  console.print("#{name}: #{files.read_text("README.md")} #{files.list(".")}")
  console.print("#{name}: #{files.exists?(".")} #{files.exists?("README.md")}")
  console.print("#{name}: #{files.within(".").read_bytes("README.md").map(fn(b) b.size end)}")
end

fn main(console: Console, files: Files)
  report(console, "parent", files.within("..").read_only)
  report(console, "through", files.within("tests/../..").read_only)
  report(console, "nested", files.within("tests").within("../..").read_only)
  inside = files.within("tests/golden/cap-files-within-escape/data").read_only
  console.print("#{inside.exists?("note.txt")} #{inside.read_text("../README.md")}")
  nothing = files.within("..")
  console.print("#{nothing.write_text("x.txt", "x")} #{nothing.make_folder("d")}")
  console.print("#{nothing.remove("x.txt")} #{nothing.entries(".")} #{nothing.walk(".")}")
end
