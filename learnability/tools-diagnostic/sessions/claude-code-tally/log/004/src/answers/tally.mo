## Reports how often agents fixed Mo's compile errors on their next try.
##
## Usage: tally LOGS, where LOGS is a folder holding one folder per session.
module Tally
use Json{Json}

## One error: its code, the file of its primary span, and its function.
struct ErrorKey
  code: Text
  file: Text
  function: Text
end

## The tests a `test` run counted.
struct Totals
  passed: Int
  all: Int
end

## One run of `mo check`, `mo test` or `mo fix` in a session.
enum Run
  CheckRun(errors: Set(ErrorKey))
  TestRun(errors: Set(ErrorKey), totals: Totals)
  FixRun
end

## What became of an error that started.
enum Outcome
  Attempts(Int)
  ByFix
  Unfixed
end

## The counts for one line of the table.
struct Row
  errors: Int
  first_retry: Int
  within_three: Int
  by_fix: Int
  unfixed: Int
end

## A diagnostic record, before its function is looked up.
struct Diagnostic
  code: Text
  file: Text
  line: Int
end

## What an `output.jsonl` holds.
struct Parsed
  diagnostics: List(Diagnostic)
  totals: Totals
  bad_lines: List(Int)
end

## The sessions read from LOGS, and the lines that weren't JSON.
struct Loaded
  sessions: List(List(Run))
  complaints: List(Text)
end

fn errors_of(run: Run) : Option(Set(ErrorKey))
  case run
    CheckRun(errors: e): Some(e)
    TestRun(errors: e, totals: _): Some(e)
    FixRun: None
  end
end

## What became of `key`, which started at the run at position `start`.
fn outcome(runs: List(Run), start: Int, key: ErrorKey) : Outcome
  var attempts = 0
  var fix_seen? = false
  for run in runs.drop(start + 1)
    case errors_of(run)
      None: fix_seen? = true
      Some(errors):
        attempts += 1
        return if fix_seen?: ByFix else: Attempts(attempts) if not errors.has?(key)
        fix_seen? = false
    end
  end
  Unfixed
end

## The code and outcome of every error that started in one session.
fn started(runs: List(Run)) : List((Text, Outcome))
  var found = []
  var previous = Set.empty
  for (i, run) in runs.enumerate
    case errors_of(run)
      None: continue
      Some(errors):
        for key in errors
          if not previous.has?(key)
            found = found.append((key.code, outcome(runs, i, key)))
          end
        end
        previous = errors
    end
  end
  found
end

fn empty_row() : Row
  Row(errors: 0, first_retry: 0, within_three: 0, by_fix: 0, unfixed: 0)
end

fn counted(row: Row, result: Outcome) : Row
  var next = row
  next.errors += 1
  case result
    Attempts(n):
      if n == 1
        next.first_retry += 1
      end
      if n <= 3
        next.within_three += 1
      end
    ByFix: next.by_fix += 1
    Unfixed: next.unfixed += 1
  end
  next
end

fn row_line(label: Text, row: Row) : Text
  [
    label,
    "#{row.errors}",
    "#{row.first_retry}",
    "#{row.within_three}",
    "#{row.by_fix}",
    "#{row.unfixed}"
  ].join("\t")
end

## `part` over `whole` as a percentage with one decimal place, halves rounded up.
fn percent(part: Int, whole: Int) : Text
  examples
    percent(1, 2) == "50.0%"
    percent(2, 3) == "66.7%"
    percent(1, 16) == "6.3%"
    percent(1, 8) == "12.5%"
    percent(3, 3) == "100.0%"
    percent(0, 0) == "n/a"
  end

  return "n/a" if whole == 0
  tenths = (part * 2000 + whole) / (whole * 2)
  "#{tenths / 10}.#{tenths % 10}%"
end

## The last `test` run's totals of a session, or none.
fn last_totals(runs: List(Run)) : Totals
  var totals = Totals(passed: 0, all: 0)
  for run in runs
    case run
      TestRun(errors: _, totals: t): totals = t
      CheckRun(errors: _) | FixRun: continue
    end
  end
  totals
end

## The table's lines for these sessions.
fn report(sessions: List(List(Run))) : List(Text)
  var rows = {}
  var total = empty_row()
  var passed = 0
  var all = 0
  for runs in sessions
    for (code, result) in started(runs)
      rows = rows.update(code, empty_row(), fn(row) counted(row, result) end)
      total = counted(total, result)
    end
    totals = last_totals(runs)
    passed += totals.passed
    all += totals.all
  end
  var lines = ["code\terrors\tfirst_retry\twithin_three\tby_fix\tunfixed"]
  for code in rows.keys.sort
    lines = lines.append(row_line(code, rows.get(code) or empty_row()))
  end
  lines = lines.append(row_line("total", total))
  attempted = total.errors - total.by_fix
  lines = lines.append("first_retry_rate\t#{percent(total.first_retry, attempted)}")
  lines = lines.append("within_three_rate\t#{percent(total.within_three, attempted)}")
  lines.append("tests_passed\t#{passed}/#{all} #{percent(passed, all)}")
end

fn field(json: Json, name: Text) : Option(Json)
  case json
    Object(fields): fields.get(name)
    Null | Boolean(_) | Number(_) | String(_) | Array(_): None
  end
end

fn text_field(json: Json, name: Text) : Option(Text)
  case field(json, name)
    Some(String(t)): Some(t)
    Some(_) | None: None
  end
end

fn int_field(json: Json, name: Text) : Option(Int)
  case field(json, name)
    Some(Number(n)): n.to_int
    Some(_) | None: None
  end
end

fn diagnostic(json: Json) : Option(Diagnostic)
  code = try text_field(json, "code")
  primary = try field(json, "primary")
  file = try text_field(primary, "file")
  line = try int_field(primary, "line")
  Some(Diagnostic(code: code, file: file, line: line))
end

## Reads what `mo check --json` or `mo test --json` printed.
fn parse_output(text: Text) : Parsed
  var diagnostics = []
  var totals = Totals(passed: 0, all: 0)
  var bad_lines = []
  for (i, line) in text.lines.enumerate
    case Json.parse(line)
      Error(_): bad_lines = bad_lines.append(i + 1)
      Ok(json):
        record = text_field(json, "record") or ""
        if record == "diagnostic"
          if diagnostic(json) is Some(d)
            diagnostics = diagnostics.append(d)
          end
        else if record == "totals"
          passed = int_field(json, "passed") or 0
          failed = int_field(json, "failed") or 0
          stopped = int_field(json, "stopped") or 0
          totals = Totals(passed: passed, all: passed + failed + stopped)
        end
    end
  end
  Parsed(diagnostics: diagnostics, totals: totals, bad_lines: bad_lines)
end

## The function holding line `line` of `source`, as the rules name it.
fn function_at(source: Text, line: Int) : Text
  examples
    function_at("module M\n\nfn f(x: Int)\n  x\nend\n", 4) == "f"
    function_at("module M\n\nfn f(x: Int)\n  x\nend\n", 2) == "(module)"
    function_at("fn pick[T](x: T)\ntest \"it works\"  \n  x\n", 3) == "test \"it works\""
    function_at("fn pick[T](x: T)\n  fn inner(y)\n", 2) == "pick"
  end

  var found = "(module)"
  for (i, text) in source.lines.enumerate
    break if i >= line
    if text.drop_prefix("fn ") is Some(rest)
      found = (rest.split("(").first or "").split("[").first or ""
    else if text.starts_with?("test ")
      found = text.trim
    end
  end
  found
end

fn join(folder: Text, name: Text) : Text
  if folder == "." or folder.empty?: name else: "#{folder}/#{name}"
end

## The paths a diagnostic's file may have under `src/`: the whole path, then shorter tails.
fn candidates(file: Text) : List(Text)
  examples
    candidates("a/b.mo") == ["a/b.mo", "b.mo"]
    candidates("/x/y.mo") == ["x/y.mo", "y.mo"]
  end

  parts = file.split("/").filter(fn(p) not p.empty? and p != "." end)
  (0..parts.size).to_list.map(fn(i) parts.drop(i).join("/") end)
end

fn read_source(files: ReadFiles, src: Text, file: Text) : Option(Text)
  for path in candidates(file)
    if files.read_text(join(src, path)) is Ok(text)
      return Some(text)
    end
  end
  None
end

## The errors of one run, with each one's function found in the run's `src/`.
fn error_keys(files: ReadFiles, run_dir: Text, diagnostics: List(Diagnostic)) : Set(ErrorKey)
  src = join(run_dir, "src")
  var sources = {}
  var keys = Set.empty
  for d in diagnostics
    if not sources.has?(d.file)
      sources = sources.set(d.file, read_source(files, src, d.file))
    end
    function = case sources.get(d.file)
      Some(Some(source)): function_at(source, d.line)
      Some(None) | None: "(module)"
    end
    keys = keys.add(ErrorKey(code: d.code, file: d.file, function: function))
  end
  keys
end

## One run, or none when it is skipped, and the lines of its output that weren't JSON.
fn load_run(files: ReadFiles, run_dir: Text) : (Option(Run), List(Text))
  command = case files.read_text(join(run_dir, "command"))
    Ok(text): text.trim
    Error(_): ""
  end
  return (Some(FixRun), []) if command == "fix"
  return (None, []) if command != "check" and command != "test"
  output_path = join(run_dir, "output.jsonl")
  text = case files.read_text(output_path)
    Ok(t): t
    Error(_): return (None, [])
  end
  parsed = parse_output(text)
  if not parsed.bad_lines.empty?
    return (None, parsed.bad_lines.map(fn(n) "#{output_path}:#{n}: not JSON" end))
  end
  errors = error_keys(files, run_dir, parsed.diagnostics)
  run = if command == "check": CheckRun(errors: errors) else: TestRun(errors: errors, totals: parsed.totals)
  (Some(run), [])
end

fn run_number(entry: Entry) : Option(Int)
  return None if entry.kind != Folder
  return None if not entry.name.chars.all?(fn(c) c.ascii_digit? end)
  entry.name.to_int
end

## Every session in the folder `root`.
fn load(files: ReadFiles, root: Text) : Result(Loaded, FileError)
  var sessions = []
  var complaints = []
  for session in try files.entries(root)
    continue if session.kind != Folder
    session_dir = join(root, session.name)
    numbered = (try files.entries(session_dir)).filter_map(fn(entry)
      run_number(entry).map(fn(n) (n, entry.name) end)
    end)
    var runs = []
    for (_, name) in numbered.sort
      (run, bad) = load_run(files, join(session_dir, name))
      complaints = complaints.concat(bad)
      if run is Some(r)
        runs = runs.append(r)
      end
    end
    sessions = sessions.append(runs)
  end
  Ok(Loaded(sessions: sessions, complaints: complaints))
end

fn main(args: Args, console: Console, files: ReadFiles) : Int
  case args.list
    [logs]:
      var root = logs
      while root.byte_count > 1 and root.ends_with?("/")
        root = root.drop_suffix("/") or root
      end
      case load(files, root)
        Ok(loaded):
          for complaint in loaded.complaints
            console.error(complaint)
          end
          for line in report(loaded.sessions)
            console.print(line)
          end
          if loaded.complaints.empty?: 0 else: 1
        Error(e):
          console.error("tally: #{e}")
          2
      end
    _:
      console.error("usage: tally LOGS")
      2
  end
end
