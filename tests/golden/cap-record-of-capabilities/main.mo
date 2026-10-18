module Main

struct Tools
  console: Console
  files: ReadFiles
end

fn show(tools: Tools, path: Text)
  case tools.files.read_text(path)
    Ok(text): tools.console.print("#{path} has #{text.lines.size} lines")
    Error(failure): tools.console.print("cannot read: #{failure}")
  end
end

test "a record carries both capabilities to a helper"
  console = Console.fake()
  files = Files.in_memory({"a.txt": "x\ny\nz\n", "b.txt": ""}).read_only
  tools = Tools(console: console, files: files)
  show(tools, "a.txt")
  show(tools, "b.txt")
  show(tools, "c.txt")
  assert console.printed == ["a.txt has 3 lines", "b.txt has 0 lines", "cannot read: c.txt: no such file"]
end
