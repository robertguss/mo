## notes-tool: the handler of an MCP tool, `search_notes`, that searches the
## Markdown notes in one read-only folder and in the folders inside it.
##
## A note is a file whose name ends in `.md` or `.markdown`, in any case. A
## line of a note is a hit when it holds every word of the query, in any order
## and ignoring case. Notes are searched in the order of their names, with a
## folder's notes where the folder's name comes, and each from its first line.
## Hits stop at the limit the caller gives, from 1 to 100, and the output says
## whether more were cut there. Names that start with `.`, such as `.git` or
## `.trash`, are hidden and skipped.
##
## The handler can only read the folder it is handed. The tool's server
## narrows its own capability to the notes before it calls the handler, as in
## `search_notes(files.within("notes").read_only, input)`.
module NotesTool
expose SearchInput(..), SearchOutput(..), Hit(..), SearchError(..), search_notes, max_limit
use Unicode16

## The most hits one search returns.
max_limit = 100

## How many folders deep inside the notes folder a note may be. A deeper
## folder is refused, which also ends a walk that a link from a folder to one
## around it would make endless.
max_depth = 20

## What the tool is called with.
struct SearchInput
  query: Text  # the words to look for, separated by spaces
  limit: Int   # the most hits to return, from 1 to max_limit
end

## A line of a note that holds every word of the query.
struct Hit
  file: Text  # the note's path inside the notes folder, such as "projects/mo.md"
  line: Int   # the line's number in the note, counting from 1
  text: Text  # the line as the note has it, without its line ending
end

## What the tool returns: the first hits, and whether more were cut at the limit.
struct SearchOutput
  hits: List(Hit)    # in the order the notes were searched, at most the limit
  truncated?: Bool   # whether the notes hold more hits than the limit let through
end

## Why a search failed.
enum SearchError
  EmptyQuery                   # the query has no words
  LimitOutOfRange(limit: Int)  # the limit is below 1 or above max_limit
  TooDeep(folder: Text)        # a folder is deeper inside the notes folder than max_depth
  Unreadable(FileError)        # the notes folder, a folder in it, or a note can't be read
end

## Searches the notes for the lines that hold every word of the input's query:
## the tool's handler.
fn search_notes(notes: ReadFiles, input: SearchInput) : Result(SearchOutput, SearchError)
  promise "never more hits than the limit, a cut only when the hits fill it, and hits only from notes"
    result is Ok(output) implies output.hits.size <= input.limit and
      (output.truncated? implies output.hits.size == input.limit) and
      output.hits.all?(fn(hit) markdown?(hit.file) end)
    allows notes: Files.in_memory({"a.md": "x\nx"}).read_only,
      input: SearchInput(query: "x", limit: 1),
      result: Ok(SearchOutput(hits: [Hit(file: "a.md", line: 1, text: "x")], truncated?: true))
    allows notes: Files.in_memory({"a.md": "x"}).read_only,
      input: SearchInput(query: "x", limit: 0),
      result: Error(LimitOutOfRange(limit: 0))
    forbids notes: Files.in_memory({"a.md": "x\nx"}).read_only,
      input: SearchInput(query: "x", limit: 1),
      result: Ok(SearchOutput(hits: [], truncated?: true))
    forbids notes: Files.in_memory({"a.txt": "x"}).read_only,
      input: SearchInput(query: "x", limit: 1),
      result: Ok(SearchOutput(hits: [Hit(file: "a.txt", line: 1, text: "x")], truncated?: false))
  end

  terms = query_terms(input.query)
  return Error(EmptyQuery) if terms.empty?
  limit = try checked_limit(input.limit)
  names = try notes.list(".")
  paths = try note_paths(notes, ".", names, 0)
  # Collect one hit past the limit, to know whether any were cut.
  var hits = []
  for path in paths
    content = try notes.read_text(path)
    text = content.drop_prefix("\u{FEFF}") or content  # a byte order mark isn't part of the first line
    hits = hits.concat(hits_in(path, text, terms, limit + 1 - hits.size))
    break if hits.size > limit
  end
  Ok(SearchOutput(hits: hits.take(limit), truncated?: hits.size > limit))
end

## The words of a query, folded as `folded` does, each once.
fn query_terms(query: Text) : List(Text)
  examples
    query_terms("Launch  PLAN launch") == ["launch", "plan"]
    query_terms(" \t ") == []
  end

  folded(query).words.unique
end

## The limit, when it is one the tool accepts.
fn checked_limit(limit: Int) : Result(Int, SearchError)
  examples
    checked_limit(1) == Ok(1)
    checked_limit(100) == Ok(100)
    checked_limit(0) == Error(LimitOutOfRange(limit: 0))
    checked_limit(101) == Error(LimitOutOfRange(limit: 101))
  end

  if limit >= 1 and limit <= max_limit: Ok(limit) else: Error(LimitOutOfRange(limit: limit))
end

## The paths of the notes among `names`, the names in `folder`, and of the
## notes in the folders among them. `folder` is `depth` folders deep.
fn note_paths(notes: ReadFiles, folder: Text, names: List(Text), depth: Int) : Result(List(Text), SearchError)
  var paths = []
  for name in names
    continue if name.starts_with?(".")  # hidden, such as .git or .trash
    path = inside(folder, name)
    if markdown?(name)
      paths = paths.append(path)
    else if notes.list(path) is Ok(inner) and not inner.empty?
      return Error(TooDeep(folder: path)) if depth == max_depth
      paths = paths.concat(try note_paths(notes, path, inner, depth + 1))
    end
    # Any other name is a file that isn't a note, such as a picture, which
    # can't be listed as a folder can. A folder that can't be listed looks
    # the same from here, so it is skipped too.
  end
  Ok(paths)
end

## The path of `name`, which is in `folder`, from the notes folder.
fn inside(folder: Text, name: Text) : Text
  examples
    inside(".", "ideas.md") == "ideas.md"
    inside("projects/mo", "plan.md") == "projects/mo/plan.md"
  end

  if folder == ".": name else: "#{folder}/#{name}"
end

## Whether a file's name is a Markdown note's.
fn markdown?(name: Text) : Bool
  examples
    markdown?("ideas.md")
    markdown?("Plan.MD")
    markdown?("old.markdown")
    not markdown?("photo.png")
    not markdown?("md")
  end

  lowered = name.ascii_lowercase
  lowered.ends_with?(".md") or lowered.ends_with?(".markdown")
end

## The hits among a note's lines: the first lines that hold every term, at
## most `room` of them.
fn hits_in(file: Text, text: Text, terms: List(Text), room: Int) : List(Hit)
  promise "the first matching lines, up to room of them, each with its file, number and text"
    result.map(fn(hit) hit.line end) == text.lines.enumerate
      .filter(fn(pair) matches?(pair.1, terms) end)
      .map(fn(pair) pair.0 + 1 end)
      .take(room.max(0))
    result.all?(fn(hit) hit.file == file and text.lines.get(hit.line - 1) == Some(hit.text) end)
    allows file: "a.md", text: "one\ntwo", terms: ["two"], room: 5,
      result: [Hit(file: "a.md", line: 2, text: "two")]
    forbids file: "a.md", text: "one\ntwo", terms: ["two"], room: 5,
      result: [Hit(file: "a.md", line: 1, text: "two")]
    forbids file: "a.md", text: "one\ntwo", terms: ["two"], room: 0,
      result: [Hit(file: "a.md", line: 2, text: "two")]
  end

  var hits = []
  for (index, line) in text.lines.enumerate
    break if hits.size >= room
    continue if not matches?(line, terms)
    hits = hits.append(Hit(file: file, line: index + 1, text: line))
  end
  hits
end

## Whether a line holds every one of the terms, which are already folded.
fn matches?(line: Text, terms: List(Text)) : Bool
  examples
    matches?("Plan the *Launch*", ["launch", "plan"])
    not matches?("Plan B", ["launch", "plan"])
  end

  lowered = folded(line)
  terms.all?(fn(term) lowered.contains?(term) end)
end

## Text in the one form that searches compare: composed, as NFC has it, and
## in lowercase, beyond ASCII too.
fn folded(text: Text) : Text
  examples
    folded("ÉTÉ") == "été"
    folded("CAFE\u{301}") == "caf\u{E9}"
  end

  Unicode16.lowercase(Unicode16.normalize_nfc(text))
end

## A failure's text, for whoever called the tool to read.
fn to_text(failure: SearchError) : Text
  case failure
    EmptyQuery: "the query has no words to search for"
    LimitOutOfRange(limit: n): "the limit must be from 1 to #{max_limit}, not #{n}"
    TooDeep(folder: path): "#{path}: more than #{max_depth} folders deep"
    Unreadable(problem): "can't read the notes: #{problem}"
  end
end

test "a search finds lines in the notes and in the folders inside them, in any case"
  notes = Files.in_memory({
    "ideas.md": "# Ideas\nA search tool for Mo\n",
    "photo.png": "mo",
    "projects/mo.md": "Mo has no null\nNo tools yet\n"
  }).read_only
  assert search_notes(notes, SearchInput(query: "MO", limit: 10)) is Ok(output)
  assert output.hits == [
    Hit(file: "ideas.md", line: 2, text: "A search tool for Mo"),
    Hit(file: "projects/mo.md", line: 1, text: "Mo has no null")
  ]
  assert not output.truncated?
end

test "a hit holds every word of the query, in any order"
  notes = Files.in_memory({"plans.md": "Plan the launch\nlaunch day\nPLAN B\nthe LAUNCH plan\n"}).read_only
  assert search_notes(notes, SearchInput(query: "launch plan", limit: 10)) is Ok(output)
  assert output.hits.map(fn(hit) hit.line end) == [1, 4]
end

test "hits past the limit are cut, and the output says so"
  notes = Files.in_memory({"a.md": "x 1\nx 2", "b.md": "x 3"}).read_only
  assert search_notes(notes, SearchInput(query: "x", limit: 2)) is Ok(cut)
  assert cut.hits == [Hit(file: "a.md", line: 1, text: "x 1"), Hit(file: "a.md", line: 2, text: "x 2")]
  assert cut.truncated?
  assert search_notes(notes, SearchInput(query: "x", limit: 3)) is Ok(whole)
  assert whole.hits.size == 3
  assert not whole.truncated?
end

test "hidden folders and files that aren't notes are skipped"
  notes = Files.in_memory({
    ".trash/old.md": "plan",
    "plan.txt": "plan",
    "today.md": "plan"
  }).read_only
  assert search_notes(notes, SearchInput(query: "plan", limit: 10)) is Ok(output)
  assert output.hits == [Hit(file: "today.md", line: 1, text: "plan")]
end

test "a byte order mark isn't part of a note's first line"
  notes = Files.in_memory({"a.md": "\u{FEFF}Mo notes"}).read_only
  assert search_notes(notes, SearchInput(query: "mo", limit: 5)) is Ok(output)
  assert output.hits == [Hit(file: "a.md", line: 1, text: "Mo notes")]
end

test "a query with no words, and a limit outside 1 to 100, are refused"
  notes = Files.in_memory({"a.md": "x"}).read_only
  assert search_notes(notes, SearchInput(query: " ", limit: 10)) == Error(EmptyQuery)
  assert search_notes(notes, SearchInput(query: "x", limit: 0)) == Error(LimitOutOfRange(limit: 0))
  assert search_notes(notes, SearchInput(query: "x", limit: 101)) == Error(LimitOutOfRange(limit: 101))
end

test "a notes folder that can't be read gives a typed error"
  nowhere = Files.in_memory({"a.md": "x"}).read_only.within("..")
  assert search_notes(nowhere, SearchInput(query: "x", limit: 10)) is Error(Unreadable(NotAllowed(path: _)))
end

test "notes may be up to max_depth folders deep, and deeper folders are refused"
  deepest = Files.in_memory({"#{"a/".repeat(max_depth)}x.md": "x"}).read_only
  assert search_notes(deepest, SearchInput(query: "x", limit: 1)) is Ok(found)
  assert found.hits.size == 1
  too_deep = Files.in_memory({"#{"a/".repeat(max_depth + 1)}x.md": "x"}).read_only
  assert search_notes(too_deep, SearchInput(query: "x", limit: 1)) is Error(TooDeep(folder: _))
end

test "a failure reads as a sentence"
  assert "#{LimitOutOfRange(limit: 500)}" == "the limit must be from 1 to 100, not 500"
  assert "#{Unreadable(NotFound(path: "a.md"))}" == "can't read the notes: a.md: no such file"
end
