## Reports how often agents fixed Mo's compile errors on their next try.
## Usage: tally LOGS, where LOGS holds one folder per session.
module Tally
use Json{Json}

## An error, as its code, the file of its primary span, and its function.
struct ErrorKey
  code: Text
  file: Text
  function: Text
end

## The tests one `test` run counted.
struct Totals
  passed: Int
  run: Int
end

## A run of a session: a `check` or `test` run with the errors it reported, or a `fix`.
enum Run
  Checked(errors: Set(ErrorKey), tests: Option(Totals))
  Fixed
end

## What became of one started error.
enum Outcome
  Attempts(Int)
  ByFix
  Unfixed
end

## An error still reported, with the attempts made so far.
struct Open
  attempts: Int
  fix?: Bool
end

## The counts of one row of the table.
struct Counts
  errors: Int
  first_retry: Int
  within_three: Int
  by_fix: Int
  unfixed: Int
end

## What reading a run gives.
enum Loaded
  Read(Run)
  Ignored
  Skipped(messages: List(Text))
end

fn main(args: Args, console: Console, files: ReadFiles) : Int
  logs = case args.list
    [one]: one
    _:
      console.error("usage: tally LOGS")
      return 2
  end
  root = logs.drop_suffix("/") or logs
  sessions = case files.entries(root)
    Ok(entries): entries
    Error(problem):
      console.error("#{problem}")
      return 2
  end
  var status = 0
  var counts = {}
  var passed = 0
  var ran = 0
  for session in sessions
    continue if session.kind != Folder
    folder = "#{root}/#{session.name}"
    run_names = case files.entries(folder)
      Ok(entries): run_folders(entries)
      Error(problem):
        console.error("#{problem}")
        return 2
    end
    var runs = []
    for name in run_names
      case load_run(files, "#{folder}/#{name}")
        Read(run): runs = runs.append(run)
        Ignored:
        Skipped(messages: messages):
          for message in messages
            console.error(message)
          end
          status = 1
      end
    end
    for (code, outcome) in outcomes(runs)
      counts = counts.update(code, zero(), fn(c) add(c, outcome) end)
    end
    if last_tests(runs) is Some(totals)
      passed += totals.passed
      ran += totals.run
    end
  end
  for line in table(counts, passed, ran)
    console.print(line)
  end
  status
end

## The names of the run folders among a session's entries, in the order of their numbers.
fn run_folders(entries: List(Entry)) : List(Text)
  entries
    .filter(fn(entry) entry.kind == Folder and run_number(entry.name) != None end)
    .map(fn(entry) entry.name end)
    .sort_by(fn(name) run_number(name) or 0 end)
end

## The number of a run folder's name: three digits or more.
fn run_number(name: Text) : Option(Int)
  examples
    run_number("001") == Some(1)
    run_number("120") == Some(120)
    run_number("01") == None
    run_number("-01") == None
    run_number("notes") == None
  end

  return None if name.char_count < 3
  return None if not name.chars.all?(fn(c) c.ascii_digit? end)
  name.to_int
end

## Reads the run in a folder.
fn load_run(files: ReadFiles, folder: Text) : Loaded
  command_path = "#{folder}/command"
  command = case files.read_text(command_path)
    Ok(text): text.trim
    Error(problem): return Skipped(messages: ["#{problem}"])
  end
  return Read(Fixed) if command == "fix"
  return Ignored if command != "check" and command != "test"
  output_path = "#{folder}/output.jsonl"
  output = case files.read_text(output_path)
    Ok(text): text
    Error(problem): return Skipped(messages: ["#{problem}"])
  end
  case parse_lines(output, output_path)
    Ok(records): Read(checked(files, folder, records, command == "test"))
    Error(messages): Skipped(messages: messages)
  end
end

## The JSON value of each line, or a message for each line that isn't JSON.
fn parse_lines(text: Text, path: Text) : Result(List(Json), List(Text))
  examples
    parse_lines("{\"a\": 1}\n[]\n", "o") == Ok([Json.parse("{\"a\": 1}") or Json.Null, Json.Array([])])
    parse_lines("{}\nnope\n{\n", "x/o") == Error(["x/o:2: not JSON", "x/o:3: not JSON"])
  end

  var records = []
  var bad = []
  for (i, line) in text.lines.enumerate
    case Json.parse(line)
      Ok(json): records = records.append(json)
      Error(_): bad = bad.append("#{path}:#{i + 1}: not JSON")
    end
  end
  if bad.empty?: Ok(records) else: Error(bad)
end

## The run that a `check` or `test` run's records describe.
fn checked(files: ReadFiles, folder: Text, records: List(Json), test?: Bool) : Run
  var errors = Set.empty
  var totals = Totals(passed: 0, run: 0)
  for record in records
    kind = text_field(record, "record") or ""
    if kind == "diagnostic"
      case diagnostic_place(record)
        Some((code, file, line)):
          source = case files.read_text("#{folder}/src/#{file}")
            Ok(text): text
            Error(_): ""
          end
          errors = errors.add(ErrorKey(code: code, file: file, function: function_at(source, line)))
        None:
      end
    else if kind == "totals"
      passed = int_field(record, "passed") or 0
      failed = int_field(record, "failed") or 0
      stopped = int_field(record, "stopped") or 0
      totals = Totals(passed: passed, run: passed + failed + stopped)
    end
  end
  Checked(errors: errors, tests: if test?: Some(totals) else: None)
end

## A diagnostic's code, and the file and line of its primary span.
fn diagnostic_place(record: Json) : Option((Text, Text, Int))
  code = try text_field(record, "code")
  primary = try field(record, "primary")
  file = try text_field(primary, "file")
  line = try int_field(primary, "line")
  Some((code, file, line))
end

fn field(json: Json, name: Text) : Option(Json)
  case json
    Object(fields): fields.get(name)
    Null | Boolean(_) | Number(_) | String(_) | Array(_): None
  end
end

fn text_field(json: Json, name: Text) : Option(Text)
  case field(json, name)
    Some(String(text)): Some(text)
    Some(_) | None: None
  end
end

fn int_field(json: Json, name: Text) : Option(Int)
  case field(json, name)
    Some(Number(number)): number.to_int
    Some(_) | None: None
  end
end

## The function a line of source is in: the last `fn` or `test` line at or before it.
fn function_at(source: Text, line: Int) : Text
  examples
    function_at("module A\n\nfn first(x: Int)\n  x\n", 4) == "first"
    function_at("module A\n\nfn first(x: Int)\n  x\n", 3) == "first"
    function_at("module A\n\nfn first(x: Int)\n  x\n", 2) == "(module)"
    function_at("fn a()\nend\ntest \"it works\"  \n  assert a()\n", 4) == "test \"it works\""
    function_at("fn largest[T](items: List(T))\n  items\n", 2) == "largest"
    function_at("  fn inner()\n", 1) == "(module)"
  end

  var found = "(module)"
  for (i, text) in source.lines.enumerate
    break if i >= line
    if text.starts_with?("fn ")
      found = function_name(text)
    else if text.starts_with?("test ")
      found = text.trim
    end
  end
  found
end

## The word after `fn`, up to `(` or `[`.
fn function_name(text: Text) : Text
  examples
    function_name("fn main(args: Args)") == "main"
    function_name("fn pick[T](x: T)") == "pick"
    function_name("fn bare") == "bare"
  end

  rest = text.drop_prefix("fn ") or text
  var name = []
  for c in rest.chars
    break if c == '(' or c == '['
    name = name.append(c)
  end
  Text.from_chars(name).trim
end

## What became of each error a session's runs started, with its code.
fn outcomes(runs: List(Run)) : List((Text, Outcome))
  var open = {}
  var done = []
  for run in runs
    case run
      Fixed:
        var marked = {}
        for (key, state) in open
          marked = marked.set(key, Open(attempts: state.attempts, fix?: true))
        end
        open = marked
      Checked(errors: reported, tests: _):
        var next = {}
        for (key, state) in open
          if reported.has?(key)
            next = next.set(key, Open(attempts: state.attempts + 1, fix?: false))
          else
            outcome = if state.fix?: ByFix else: Attempts(state.attempts + 1)
            done = done.append((key.code, outcome))
          end
        end
        for key in reported
          if not open.has?(key)
            next = next.set(key, Open(attempts: 0, fix?: false))
          end
        end
        open = next
    end
  end
  for key in open.keys
    done = done.append((key.code, Unfixed))
  end
  done
end

## The totals of the last `test` run.
fn last_tests(runs: List(Run)) : Option(Totals)
  var last = None
  for run in runs
    case run
      Checked(errors: _, tests: Some(totals)): last = Some(totals)
      Checked(errors: _, tests: None) | Fixed:
    end
  end
  last
end

fn zero() : Counts
  Counts(errors: 0, first_retry: 0, within_three: 0, by_fix: 0, unfixed: 0)
end

## Counts with one more error.
fn add(counts: Counts, outcome: Outcome) : Counts
  var c = counts
  c.errors += 1
  case outcome
    Attempts(n):
      if n == 1
        c.first_retry += 1
      end
      if n <= 3
        c.within_three += 1
      end
    ByFix: c.by_fix += 1
    Unfixed: c.unfixed += 1
  end
  c
end

## A percentage with one decimal place, rounding halves up, or `n/a` for a whole of 0.
fn percent(part: Int, whole: Int) : Text
  examples
    percent(1, 2) == "50.0%"
    percent(40, 44) == "90.9%"
    percent(2, 3) == "66.7%"
    percent(1, 16) == "6.3%"
    percent(3, 3) == "100.0%"
    percent(0, 5) == "0.0%"
    percent(0, 0) == "n/a"
  end

  return "n/a" if whole == 0
  tenths = (part * 2000 + whole) / (2 * whole)
  "#{tenths / 10}.#{tenths % 10}%"
end

## The table's lines.
fn table(counts: Map(Text, Counts), passed: Int, ran: Int) : List(Text)
  var lines = ["code\terrors\tfirst_retry\twithin_three\tby_fix\tunfixed"]
  var total = zero()
  for code in counts.keys.sort
    c = counts.get(code) or zero()
    lines = lines.append(row(code, c))
    total = Counts(
      errors: total.errors + c.errors,
      first_retry: total.first_retry + c.first_retry,
      within_three: total.within_three + c.within_three,
      by_fix: total.by_fix + c.by_fix,
      unfixed: total.unfixed + c.unfixed
    )
  end
  lines = lines.append(row("total", total))
  attempted = total.errors - total.by_fix
  lines = lines.append("first_retry_rate\t#{percent(total.first_retry, attempted)}")
  lines = lines.append("within_three_rate\t#{percent(total.within_three, attempted)}")
  lines.append("tests_passed\t#{passed}/#{ran} #{percent(passed, ran)}")
end

fn row(name: Text, c: Counts) : Text
  "#{name}\t#{c.errors}\t#{c.first_retry}\t#{c.within_three}\t#{c.by_fix}\t#{c.unfixed}"
end
