## Typed notes-tool handler; the MCP host supplies its read-only notes folder.
module NotesTool
expose SearchInput(..), Hit(..), SearchOutput(..), SearchError(..), handle

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
  FileFailure(FileError)
end

## Searches Markdown files directly inside notes, in filename and line order.
## The host must scope notes to its configured folder; input cannot select paths.
## Matches are case-sensitive literal substrings, preserving query whitespace.
## Each matching line yields one hit, with a relative file and one-based line.
## Truncation means an additional matching line actually exists, not just that
## the limit was reached. Any listing or candidate read failure rejects the call.
## MCP transport and schema registration are supplied by the host, not Mo v0.1.
fn handle(input: SearchInput, notes: ReadFiles) : Result(SearchOutput, SearchError)
  return Error(EmptyQuery) if input.query.trim.empty?
  return Error(InvalidLimit(value: input.limit)) if input.limit < 1 or input.limit > 100

  names = try notes.list(".")
  var hits = []
  var truncated? = false
  for name in names
    extension_name = name.ascii_lowercase
    continue if not (extension_name.ends_with?(".md") or extension_name.ends_with?(".markdown"))
    content = try notes.read_text(name)
    for (index, text) in content.lines.enumerate
      continue if not text.contains?(input.query)
      if hits.size < input.limit
        hits = hits.append(Hit(file: name, line: index + 1, text: text))
      else
        truncated? = true
      end
    end
  end
  Ok(SearchOutput(hits: hits, truncated?: truncated?))
end
