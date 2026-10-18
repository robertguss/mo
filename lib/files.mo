module Files
expose walk

## Every entry below folder, each named by its path from folder: a folder's entries in list's
## order, each folder followed by what's inside it. It doesn't enter links, and stops at the
## first folder it can't read.
fn walk(files: Files, folder: Text) : Result(List(Entry), FileError)
  examples
    Files.in_memory({"b.txt": "", "a/c.txt": "", "a/d/e.txt": ""}).walk(".") ==
      Ok([
        Entry(name: "a", kind: Folder),
        Entry(name: "a/c.txt", kind: File),
        Entry(name: "a/d", kind: Folder),
        Entry(name: "a/d/e.txt", kind: File),
        Entry(name: "b.txt", kind: File)
      ])
    Files.in_memory({"a/b/c.txt": ""}).walk("a") ==
      Ok([Entry(name: "b", kind: Folder), Entry(name: "b/c.txt", kind: File)])
    Files.in_memory({}).walk(".") == Ok([])
    Files.in_memory({}).walk("gone") == Error(NotFound(path: "gone"))
  end

  walk_below(files, folder, "")
end

## The entries below prefix, a path from folder, or below folder itself when prefix is "".
fn walk_below(files: Files, folder: Text, prefix: Text) : Result(List(Entry), FileError)
  var found = []
  for entry in try files.entries(below(folder, prefix))
    name = below(prefix, entry.name)
    found = found.append(Entry(name: name, kind: entry.kind))
    case entry.kind
      Folder: found = found.concat(try walk_below(files, folder, name))
      File: Unit
      Link: Unit
      Other: Unit
    end
  end
  Ok(found)
end

## The path to name inside folder, where "" and "." stand for the folder itself.
fn below(folder: Text, name: Text) : Text
  if name == ""
    folder
  else if folder == "" or folder == "."
    name
  else
    "#{folder}/#{name}"
  end
end
