## The handler of notes-tool, an MCP tool that searches the Markdown notes in
## one read-only folder.
##
## A call gives a query and a limit. The handler reads the notes, which are the
## `.md` and `.markdown` files directly in the folder, in name order, and
## returns the lines that hold the query, ignoring case, each with its file and
## line number. It returns at most `limit` hits, says whether more were cut,
## and reads no further than the first hit past the limit. Bad input, or a note
## that can't be read, gives a typed error instead.
module NotesTool
expose Input(..), Output(..), Hit(..), SearchError(..), handle, max_limit
use Unicode16

## The most hits a call may ask for.
max_limit = 100

## What a call asks for.
struct Input
  query: Text  # found within single lines, ignoring case; white space around it is dropped
  limit: Int   # the most hits to return, from 1 to max_limit
end

## One line of a note that holds the query.
struct Hit
  file: Text  # the note's file name in the folder
  line: Int   # the line's number, counting from 1
  text: Text  # the whole line, without its line break
end

## What a search found.
struct Output
  hits: List(Hit)   # in file name order, then line order
  truncated?: Bool  # whether more lines held the query than the limit let through
end

## Why a search can't be done.
enum SearchError
  EmptyQuery                   # the query is empty, or only white space
  QueryHasLineBreak            # notes are searched one line at a time
  LimitOutOfRange(limit: Int)  # the limit isn't from 1 to max_limit
  Unreadable(FileError)        # the folder, or a note in it, can't be read as text
end

## How a failure reads to the tool's caller.
fn to_text(failure: SearchError) : Text
  examples
    "#{EmptyQuery}" == "the query is empty"
    "#{LimitOutOfRange(limit: 500)}" == "the limit is 500, but it must be from 1 to 100"
  end

  case failure
    EmptyQuery: "the query is empty"
    QueryHasLineBreak: "the query has a line break, but notes are searched one line at a time"
    LimitOutOfRange(limit: n): "the limit is #{n}, but it must be from 1 to #{max_limit}"
    Unreadable(problem): "the notes can't be read: #{problem}"
  end
end

## Searches the notes for the input's query. `notes` is the notes' folder, as a
## `ReadFiles`, so the tool can read it and nothing else.
fn handle(input: Input, notes: ReadFiles) : Result(Output, SearchError)
  promise "never more hits than the limit, each a line of a Markdown note that holds the query, and cut only when the limit is full"
    result is Ok(output) implies
      output.hits.size <= input.limit and
      (output.truncated? implies output.hits.size == input.limit) and
      output.hits.all?(fn(hit)
        markdown?(hit.file) and hit.line >= 1 and folded(hit.text).contains?(folded(input.query.trim))
      end)
    allows input: Input(query: "tea", limit: 1), notes: Files.in_memory({}).read_only,
      result: Ok(Output(hits: [Hit(file: "a.md", line: 2, text: "Tea at four")], truncated?: true))
    forbids input: Input(query: "tea", limit: 1), notes: Files.in_memory({}).read_only,
      result: Ok(Output(
        hits: [Hit(file: "a.md", line: 1, text: "tea"), Hit(file: "a.md", line: 2, text: "tea")],
        truncated?: false
      ))
    forbids input: Input(query: "tea", limit: 1), notes: Files.in_memory({}).read_only,
      result: Ok(Output(hits: [], truncated?: true))
    forbids input: Input(query: "tea", limit: 1), notes: Files.in_memory({}).read_only,
      result: Ok(Output(hits: [Hit(file: "a.md", line: 2, text: "coffee")], truncated?: false))
  end

  query = input.query.trim
  return Error(EmptyQuery) if query.empty?
  return Error(QueryHasLineBreak) if query.contains?("\n") or query.contains?("\r")
  return Error(LimitOutOfRange(limit: input.limit)) if input.limit < 1 or input.limit > max_limit
  needle = folded(query)
  names = try notes.list(".")
  var hits = []
  for name in names.filter(markdown?)
    content = try notes.read_text(name)
    for (index, line) in content.lines.enumerate
      continue if not folded(line).contains?(needle)
      return Ok(Output(hits: hits, truncated?: true)) if hits.size == input.limit
      hits = hits.append(Hit(file: name, line: index + 1, text: line))
    end
  end
  Ok(Output(hits: hits, truncated?: false))
end

## Whether a file name is a Markdown note's: it ends in `.md` or `.markdown`,
## in any case.
fn markdown?(name: Text) : Bool
  examples
    markdown?("ideas.md")
    markdown?("Trips.MARKDOWN")
    not markdown?("kites.txt")
    not markdown?("md")
  end

  lowered = name.ascii_lowercase
  lowered.ends_with?(".md") or lowered.ends_with?(".markdown")
end

## The form a query and a line are compared in: lowercased beyond ASCII, then
## composed, so that "KITE" finds "kite", and an accent typed apart from its
## letter finds the same accented letter typed whole.
fn folded(text: Text) : Text
  examples
    folded("A KITE Festival") == "a kite festival"
    folded("CAF\u{C9}") == "caf\u{E9}"
    folded("Cafe\u{301}") == "caf\u{E9}"
  end

  Unicode16.normalize_nfc(Unicode16.lowercase(text))
end

test "lines holding the query come back with their note and line number, whatever their case"
  notes = Files.in_memory({
    "trips.md": "Packing list\nFly a kite at the beach\n",
    "ideas.md": "Kites from bin bags\n\nA KITE festival?\n",
    "kites.txt": "kites, but not in Markdown\n"
  }).read_only
  assert handle(Input(query: " kite ", limit: 10), notes) == Ok(Output(
    hits: [
      Hit(file: "ideas.md", line: 1, text: "Kites from bin bags"),
      Hit(file: "ideas.md", line: 3, text: "A KITE festival?"),
      Hit(file: "trips.md", line: 2, text: "Fly a kite at the beach")
    ],
    truncated?: false
  ))
end

test "hits stop at the limit, and say whether more were cut"
  notes = Files.in_memory({"a.md": "tea\ncake\ntea\n", "b.md": "more tea\n"}).read_only
  assert handle(Input(query: "tea", limit: 3), notes) is Ok(whole)
  assert whole.hits == [
    Hit(file: "a.md", line: 1, text: "tea"),
    Hit(file: "a.md", line: 3, text: "tea"),
    Hit(file: "b.md", line: 1, text: "more tea")
  ]
  assert not whole.truncated?
  assert handle(Input(query: "tea", limit: 2), notes) == Ok(Output(hits: whole.hits.take(2), truncated?: true))
end

test "a blank query, a query with a line break, or a limit outside 1 to 100 is refused"
  notes = Files.in_memory({"a.md": "tea\n"}).read_only
  assert handle(Input(query: " \t", limit: 10), notes) == Error(EmptyQuery)
  assert handle(Input(query: "tea\ncake", limit: 10), notes) == Error(QueryHasLineBreak)
  assert handle(Input(query: "tea", limit: 0), notes) == Error(LimitOutOfRange(limit: 0))
  assert handle(Input(query: "tea", limit: 101), notes) == Error(LimitOutOfRange(limit: 101))
  assert handle(Input(query: "tea", limit: 100), notes) is Ok(_)
end
