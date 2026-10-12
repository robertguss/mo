## Typed MCP search handler; transport and derived tool schemas are not in Mo v0.1.
## The caller supplies one already-narrowed read-only notes folder.
## Searches immediate .md files in filename/line order, folding ASCII case.
module NotesTool
expose SearchInput(..), Hit(..), SearchOutput(..), SearchError(..), search

struct SearchInput
  query: Text
  limit: Int
end

struct Hit
  file: Text
  line: Int
  text: Text
end

struct SearchOutput
  hits: List(Hit)
  truncated?: Bool
end

enum SearchError
  EmptyQuery
  InvalidLimit(value: Int)
  FolderRead(FileError)
  NoteRead(file: Text, problem: FileError)
end

fn search(input: SearchInput, notes: ReadFiles) : Result(SearchOutput, SearchError)
  needle = input.query.trim.ascii_lowercase
  return Error(EmptyQuery) if needle.empty?
  return Error(InvalidLimit(value: input.limit)) if input.limit < 1 or input.limit > 100
  names = case notes.list(".")
    Ok(value): value
    Error(problem): return Error(FolderRead(problem))
  end
  var hits = []
  for name in names
    continue if not name.ascii_lowercase.ends_with?(".md")
    text = case notes.read_text(name)
      Ok(value): value
      Error(problem): return Error(NoteRead(file: name, problem: problem))
    end
    for (index, line) in text.lines.enumerate
      continue if not line.ascii_lowercase.contains?(needle)
      if hits.size == input.limit
        return Ok(SearchOutput(hits: hits, truncated?: true))
      end
      hits = hits.append(Hit(file: name, line: index + 1, text: line))
    end
  end
  Ok(SearchOutput(hits: hits, truncated?: false))
end

test "exactly the limit is not truncated, but another match is"
  notes = Files.in_memory({"a.md": "Alpha\nno match\nalpha again\n", "skip.txt": "alpha\n"}).read_only
  assert search(SearchInput(query: "ALPHA", limit: 2), notes) == Ok(SearchOutput(
    hits: [Hit(file: "a.md", line: 1, text: "Alpha"), Hit(file: "a.md", line: 3, text: "alpha again")],
    truncated?: false))
  assert search(SearchInput(query: "alpha", limit: 1), notes) == Ok(SearchOutput(
    hits: [Hit(file: "a.md", line: 1, text: "Alpha")], truncated?: true))
  assert search(SearchInput(query: "alpha", limit: 101), notes) == Error(InvalidLimit(value: 101))
end
