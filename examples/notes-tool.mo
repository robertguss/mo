## The handler of an MCP tool that searches Markdown notes. It is handed one
## read-only notes folder, takes a typed input, and returns typed hits, or a
## typed error that the tool reports to its caller. Connecting it to MCP comes
## with services, in a later version.
module NotesTool
expose SearchInput(..), SearchOutput(..), Hit(..), ToolError(..), search_notes

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

enum ToolError
  EmptyQuery
  BadLimit(limit: Int)
  Unreadable(FileError)
end

max_limit = 100

## Finds the lines of the .md notes that contain the query, ignoring ASCII case,
## in file order and then line order.
fn search_notes(notes: ReadFiles, input: SearchInput) : Result(SearchOutput, ToolError)
  promise "a search never returns more hits than its limit"
    result is Ok(output) implies output.hits.size <= input.limit
    allows notes: Files.in_memory({}).read_only, input: SearchInput(query: "x", limit: 1), result: Ok(
      SearchOutput(hits: [], truncated?: false)
    )
    forbids notes: Files.in_memory({}).read_only, input: SearchInput(query: "x", limit: 1), result: Ok(
      SearchOutput(hits: [hit, hit], truncated?: false)
    )
  end

  query = input.query.trim.ascii_lowercase
  return Error(EmptyQuery) if query.empty?
  return Error(BadLimit(limit: input.limit)) if input.limit < 1 or input.limit > max_limit
  var hits = []
  for name in try notes.list(".")
    continue if not name.ends_with?(".md")
    text = try notes.read_text(name)
    for (index, line) in text.lines.enumerate
      if line.ascii_lowercase.contains?(query)
        hits = hits.append(Hit(file: name, line: index + 1, text: line.trim))
      end
    end
  end
  Ok(SearchOutput(hits: hits.take(input.limit), truncated?: hits.size > input.limit))
end

hit = Hit(file: "a.md", line: 1, text: "x")

test "only Markdown notes are searched, and case doesn't matter"
  notes = Files.in_memory({
    "plans.md": "# Plans\nCall the Bakery on Monday\n",
    "recipes.md": "bread: flour, water, salt\n",
    "bakery.txt": "not a note\n"
  }).read_only
  assert search_notes(notes, SearchInput(query: "bakery", limit: 10)) ==
    Ok(
      SearchOutput(
        hits: [Hit(file: "plans.md", line: 2, text: "Call the Bakery on Monday")],
        truncated?: false
      )
    )
end

test "too many hits are cut at the limit, and the output says so"
  notes = Files.in_memory({"a.md": "x\nx\nx\n"}).read_only
  assert search_notes(notes, SearchInput(query: "x", limit: 2)) is Ok(output)
  assert output.hits.size == 2 and output.truncated?
end

test "bad input is a typed error"
  no_notes = Files.in_memory({}).read_only
  assert search_notes(no_notes, SearchInput(query: "  ", limit: 5)) == Error(EmptyQuery)
  assert search_notes(no_notes, SearchInput(query: "x", limit: 0)) == Error(BadLimit(limit: 0))
end
