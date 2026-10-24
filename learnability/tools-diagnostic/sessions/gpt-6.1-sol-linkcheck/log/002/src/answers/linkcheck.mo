## Checks relative Markdown links beneath a folder.
module Linkcheck

fn markdown_files(files: ReadFiles, folder: Text) : Result(List(Text), FileError)
  entries = try files.entries(folder)
  var paths = []
  for entry in entries
    path = if folder == ".": entry.name else: "#{folder}/#{entry.name}"
    case entry.kind
      Folder:
        if not entry.name.starts_with?(".")
          paths = paths.concat(try markdown_files(files, path))
        end
      File | Link:
        if entry.name.ends_with?(".md")
          paths = paths.append(path)
        end
      Other: Unit
    end
  end
  Ok(paths.sort)
end

fn main(args: Args, console: Console, files: ReadFiles) : Int
  if args.list.size > 1
    console.error("usage: linkcheck [FOLDER]")
    return 2
  end
  folder = args.list.first or "."
  if folder.starts_with?("/") or folder.starts_with?("-") or folder.empty?
    console.error("usage: linkcheck [FOLDER]")
    return 2
  end
  scoped = files.within(folder)
  paths = case markdown_files(scoped, ".")
    Ok(found): found
    Error(problem):
      console.error("#{folder}: #{problem}")
      return 2
  end
  for path in paths
    case scoped.read_text(path)
      Ok(_): Unit
      Error(problem):
        console.error("#{path}: #{problem}")
        return 2
    end
  end
  console.print("ok: 0 links in #{paths.size} files")
  0
end

test "only Markdown files outside hidden folders are read"
  files = Files.in_memory({"README.md": "", "docs/a.md": "", ".hidden/x.md": "", "x.txt": ""})
  console = Console.fake()
  assert main(Args.of([]), console, files.read_only) == 0
  assert console.printed == ["ok: 0 links in 2 files"]
end
