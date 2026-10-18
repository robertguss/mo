module Files
expose walk

## Every entry below folder, each named by its path from folder: a folder's entries in list's
## order, each folder followed by what's inside it. It doesn't enter links, and stops at the
## first folder it can't read. It is ReadFiles.walk, on this capability read-only.
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

  files.read_only.walk(folder)
end
