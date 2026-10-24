## Counts compile-error repair attempts across recorded Mo sessions.
module Tally
use Json

struct Counts
  errors: Int
  first_retry: Int
  within_three: Int
  by_fix: Int
  unfixed: Int
end

struct ErrorKey
  code: Text
  file: Text
  function: Text
end

struct Run
  errors: Set(ErrorKey)
  passed: Int
  tests: Int
end

struct Summary
  counts: Map(Text, Counts)
  passed: Int
  tests: Int
  bad_json?: Bool
end

fn zero() : Counts
  Counts(errors: 0, first_retry: 0, within_three: 0, by_fix: 0, unfixed: 0)
end

fn add(a: Counts, b: Counts) : Counts
  Counts(
    errors: a.errors + b.errors,
    first_retry: a.first_retry + b.first_retry,
    within_three: a.within_three + b.within_three,
    by_fix: a.by_fix + b.by_fix,
    unfixed: a.unfixed + b.unfixed
  )
end

fn field(value: Json, name: Text) : Json
  if value is Json.Object(fields)
    return fields.get(name) or Json.Null
  end
  Json.Null
end

fn string(value: Json) : Text
  if value is Json.String(text)
    return text
  end
  ""
end

fn integer(value: Json) : Int
  if value is Json.Number(number)
    return number.to_int or 0
  end
  0
end

fn rate(numerator: Int, denominator: Int) : Text
  examples
    rate(0, 0) == "n/a"
    rate(3, 4) == "75.0%"
    rate(1, 16) == "6.3%"
  end

  return "n/a" if denominator == 0
  n = numerator.to_big
  d = denominator.to_big
  rounded = (n * 2000 + d) / (d * 2)
  "#{rounded / 10}.#{rounded % 10}%"
end

fn function_at(source: Text, line_number: Int) : Text
  var found = "(module)"
  for (index, line) in source.lines.enumerate
    break if index >= line_number
    if line.starts_with?("fn ")
      tail = line.drop_prefix("fn ") or ""
      found = (tail.replace("[", "(").split("(").first or "").trim
      found = found.words.first or "(module)"
    else if line.starts_with?("test ")
      found = line.trim
    end
  end
  found
end

fn load_run(files: ReadFiles, console: Console, path: Text) : Result(Option(Run), FileError)
  output_path = "#{path}/output.jsonl"
  output = try files.read_text(output_path)
  var records = []
  var bad? = false
  for (index, line) in output.lines.enumerate
    case Json.parse(line)
      Ok(record): records = records.append(record)
      Error(_problem):
        console.error("#{output_path}:#{index + 1}: not JSON")
        bad? = true
    end
  end
  return Ok(None) if bad?
  var errors = Set.empty
  var sources = {}
  var passed = 0
  var tests = 0
  for record in records
    kind = string(field(record, "record"))
    if kind == "diagnostic"
      primary = field(record, "primary")
      file = string(field(primary, "file"))
      source = case sources.get(file)
        Some(cached): cached
        None:
          loaded = try files.read_text("#{path}/src/#{file}")
          sources = sources.set(file, loaded)
          loaded
      end
      key = ErrorKey(
        code: string(field(record, "code")),
        file: file,
        function: function_at(source, integer(field(primary, "line")))
      )
      errors = errors.add(key)
    else if kind == "totals"
      passed = integer(field(record, "passed"))
      tests = passed + integer(field(record, "failed")) + integer(field(record, "stopped"))
    end
  end
  Ok(Some(Run(errors: errors, passed: passed, tests: tests)))
end

fn session(files: ReadFiles, console: Console, path: Text) : Result(Summary, FileError)
  entries = try files.entries(path)
  runs = entries.filter(fn(entry)
    entry.kind == Folder and
      entry.name.char_count == 3 and
      entry.name.chars.all?(fn(character) character.ascii_digit? end)
  end).sort_by(fn(entry) entry.name end)
  var summary = Summary(counts: {}, passed: 0, tests: 0, bad_json?: false)
  var active = {}
  var index = 0
  var fix? = false
  for entry in runs
    run_path = "#{path}/#{entry.name}"
    command = (try files.read_text("#{run_path}/command")).trim
    if command == "fix"
      fix? = true
      continue
    end
    continue if command != "check" and command != "test"
    loaded = try load_run(files, console, run_path)
    run = case loaded
      None:
        summary.bad_json? = true
        continue
      Some(value): value
    end
    index += 1
    for (key, started) in active
      if not run.errors.has?(key)
        attempts = index - started
        var counts = summary.counts.get(key.code) or zero()
        if fix?
          counts.by_fix += 1
        else
          if attempts == 1
            counts.first_retry += 1
          end
          if attempts <= 3
            counts.within_three += 1
          end
        end
        summary.counts = summary.counts.set(key.code, counts)
      end
    end
    var next_active = {}
    for key in run.errors
      started = case active.get(key)
        Some(previous): previous
        None:
          var counts = summary.counts.get(key.code) or zero()
          counts.errors += 1
          summary.counts = summary.counts.set(key.code, counts)
          index
      end
      next_active = next_active.set(key, started)
    end
    active = next_active
    fix? = false
    if command == "test"
      summary.passed = run.passed
      summary.tests = run.tests
    end
  end
  for (key, _started) in active
    var counts = summary.counts.get(key.code) or zero()
    counts.unfixed += 1
    summary.counts = summary.counts.set(key.code, counts)
  end
  Ok(summary)
end

fn tally(files: ReadFiles, console: Console, path: Text) : Result(Summary, FileError)
  entries = try files.entries(path)
  var summary = Summary(counts: {}, passed: 0, tests: 0, bad_json?: false)
  for entry in entries
    continue if entry.kind != Folder
    part = try session(files, console, "#{path}/#{entry.name}")
    for (code, counts) in part.counts
      summary.counts = summary.counts.set(code, add(summary.counts.get(code) or zero(), counts))
    end
    summary.passed += part.passed
    summary.tests += part.tests
    summary.bad_json? = summary.bad_json? or part.bad_json?
  end
  Ok(summary)
end

fn row(code: Text, counts: Counts) : Text
  "#{code}\t#{counts.errors}\t#{counts.first_retry}\t#{counts.within_three}\t#{counts.by_fix}\t#{counts
    .unfixed}"
end

fn main(args: Args, console: Console, files: ReadFiles) : Int
  if args.list.size != 1
    console.error("usage: tally LOGS")
    return 2
  end
  path = args.list.first or "."
  summary = case tally(files, console, path)
    Error(problem):
      console.error("#{problem}")
      return 2
    Ok(value): value
  end
  var total = zero()
  console.print("code\terrors\tfirst_retry\twithin_three\tby_fix\tunfixed")
  for code in summary.counts.keys.sort
    counts = summary.counts.get(code) or zero()
    console.print(row(code, counts))
    total = add(total, counts)
  end
  console.print(row("total", total))
  denominator = total.errors - total.by_fix
  console.print("first_retry_rate\t#{rate(total.first_retry, denominator)}")
  console.print("within_three_rate\t#{rate(total.within_three, denominator)}")
  console.print(
    "tests_passed\t#{summary.passed}/#{summary.tests} #{rate(summary.passed, summary.tests)}"
  )
  if summary.bad_json?: 1 else: 0
end

test "the example table deduplicates diagnostics and ignores unknown records"
  first = """
    {"record":"version","format":"1.0"}
    {"record":"diagnostic","code":"E0078","primary":{"file":"a.mo","line":3}}
    {"record":"diagnostic","code":"E0050","primary":{"file":"a.mo","line":1},"future":true}
    {"record":"diagnostic","code":"E0050","primary":{"file":"a.mo","line":2}}
    {"record":"future","anything":[1,2,3]}
    """
  second = """
    {"record":"diagnostic","code":"E0078","primary":{"file":"a.mo","line":5}}
    """
  third = """
    {"record":"result","status":"failed"}
    {"record":"totals","passed":3,"failed":1,"stopped":0,"status":1}
    """
  files = Files.in_memory({
    "logs/s/003/command": "test\n",
    "logs/s/003/output.jsonl": third,
    "logs/s/002/command": "check\n",
    "logs/s/002/output.jsonl": second,
    "logs/s/002/src/a.mo": "# moved\n# down\nfn f()\nend\nfn g()\nend\n",
    "logs/s/001/command": "check\n",
    "logs/s/001/output.jsonl": first,
    "logs/s/001/src/a.mo": "fn f()\nend\nfn g()\nend\n"
  }).read_only
  console = Console.fake()
  assert main(Args.of(["logs"]), console, files) == 0
  assert console.printed_errors == []
  assert console.printed == [
    "code\terrors\tfirst_retry\twithin_three\tby_fix\tunfixed",
    "E0050\t1\t1\t1\t0\t0",
    "E0078\t1\t0\t1\t0\t0",
    "total\t2\t1\t2\t0\t0",
    "first_retry_rate\t50.0%",
    "within_three_rate\t100.0%",
    "tests_passed\t3/4 75.0%"
  ]
end

test "usage and unreadable logs fail without printing a table"
  files = Files.in_memory({"not-folder": "text"}).read_only
  missing = Console.fake()
  assert main(Args.of([]), missing, files) == 2
  assert missing.printed == []
  assert missing.printed_errors == ["usage: tally LOGS"]
  extra = Console.fake()
  assert main(Args.of(["a", "b"]), extra, files) == 2
  assert extra.printed_errors == ["usage: tally LOGS"]
  absent = Console.fake()
  assert main(Args.of(["absent"]), absent, files) == 2
  assert absent.printed == []
  assert absent.printed_errors == ["absent: no such file"]
  not_folder = Console.fake()
  assert main(Args.of(["not-folder"]), not_folder, files) == 2
  assert not_folder.printed == []
  assert not_folder.printed_errors == ["not-folder: not a folder"]
end

test "attempt boundaries count only checks and tests and only the most recent fix"
  var data = {
    "logs/s/001/command": "fix",
    "logs/s/004/command": "fix",
    "logs/s/008/command": "fix",
    "logs/s/010/command": "fix"
  }
  for (number, codes) in [
    ("002", ["E0001", "E0003", "E0004", "E0005", "E0006"]),
    ("003", ["E0003", "E0004", "E0005", "E0006"]),
    ("005", ["E0003", "E0004", "E0005", "E0006"]),
    ("006", ["E0004", "E0005", "E0006"]),
    ("007", ["E0005", "E0006"]),
    ("009", ["E0006"])
  ]
    var lines = []
    for code in codes
      lines = lines.append(
        "{\"record\":\"diagnostic\",\"code\":\"#{code}\",\"primary\":{\"file\":\"a.mo\",\"line\":1}}"
      )
    end
    if number == "009"
      lines = lines.append("{\"record\":\"totals\",\"passed\":2,\"failed\":0,\"stopped\":1}")
    end
    data = data.set("logs/s/#{number}/command", if number == "009": "test" else: "check")
    data = data.set("logs/s/#{number}/output.jsonl", lines.join("\n"))
    data = data.set("logs/s/#{number}/src/a.mo", "fn f()\nend")
  end
  console = Console.fake()
  assert main(Args.of(["logs"]), console, Files.in_memory(data).read_only) == 0
  assert console.printed_errors == []
  assert console.printed == [
    "code\terrors\tfirst_retry\twithin_three\tby_fix\tunfixed",
    "E0001\t1\t1\t1\t0\t0",
    "E0003\t1\t0\t1\t0\t0",
    "E0004\t1\t0\t0\t0\t0",
    "E0005\t1\t0\t0\t1\t0",
    "E0006\t1\t0\t0\t0\t1",
    "total\t5\t1\t2\t1\t1",
    "first_retry_rate\t25.0%",
    "within_three_rate\t50.0%",
    "tests_passed\t2/3 66.7%"
  ]
end

test "malformed runs are absent from retries fixes and last test totals across sessions"
  diagnostic = "{\"record\":\"diagnostic\",\"code\":\"E0001\",\"primary\":{\"file\":\"a.mo\",\"line\":1}}"
  bad = """
    {"record":"totals","passed":99,"failed":0,"stopped":0}
    not JSON
    {"record":"diagnostic","code":"E9999","primary":{"file":"missing.mo","line":1}}

    {}
    """
  files = Files.in_memory({
    "logs/readme": "ignored",
    "logs/s/notes/readme": "ignored",
    "logs/s/001/command": "check",
    "logs/s/001/output.jsonl": diagnostic,
    "logs/s/001/src/a.mo": "fn f()\nend",
    "logs/s/002/command": "test",
    "logs/s/002/output.jsonl": bad,
    "logs/s/003/command": "check",
    "logs/s/003/output.jsonl": "",
    "logs/s/004/command": "check",
    "logs/s/004/output.jsonl": diagnostic,
    "logs/s/004/src/a.mo": "fn f()\nend",
    "logs/s/005/command": "fix",
    "logs/s/005/output.jsonl": "this is not a check or test",
    "logs/s/006/command": "test",
    "logs/s/006/output.jsonl": "bad",
    "logs/s/007/command": "test",
    "logs/s/007/output.jsonl": "{\"record\":\"totals\",\"passed\":2,\"failed\":1,\"stopped\":1}",
    "logs/s/008/command": "test",
    "logs/s/008/output.jsonl": "bad",
    "logs/t/001/command": "test",
    "logs/t/001/output.jsonl": "#{diagnostic}\n{\"record\":\"totals\",\"passed\":3,\"failed\":0,\"stopped\":0}",
    "logs/t/001/src/a.mo": "fn f()\nend"
  }).read_only
  console = Console.fake()
  assert main(Args.of(["logs"]), console, files) == 1
  assert console.printed_errors == [
    "logs/s/002/output.jsonl:2: not JSON",
    "logs/s/002/output.jsonl:4: not JSON",
    "logs/s/006/output.jsonl:1: not JSON",
    "logs/s/008/output.jsonl:1: not JSON"
  ]
  assert console.printed == [
    "code\terrors\tfirst_retry\twithin_three\tby_fix\tunfixed",
    "E0001\t3\t1\t1\t1\t1",
    "total\t3\t1\t1\t1\t1",
    "first_retry_rate\t50.0%",
    "within_three_rate\t50.0%",
    "tests_passed\t5/7 71.4%"
  ]
end

test "error identity includes file module generic function and complete test line"
  source = "module A\n  fn hidden()\nfn generic[T](x: T)\nend\ntest \"checks\"  \n  assert false"
  var lines = []
  for line in 1..7
    lines = lines.append(
      "{\"record\":\"diagnostic\",\"code\":\"E0010\",\"primary\":{\"file\":\"a.mo\",\"line\":#{line}}}"
    )
  end
  lines = lines.append(
    "{\"record\":\"diagnostic\",\"code\":\"E0010\",\"primary\":{\"file\":\"b.mo\",\"line\":3}}"
  )
  files = Files.in_memory({
    "logs/s/001/command": "check",
    "logs/s/001/output.jsonl": lines.join("\n"),
    "logs/s/001/src/a.mo": source,
    "logs/s/001/src/b.mo": source,
    "logs/s/002/command": "fix",
    "logs/s/003/command": "test",
    "logs/s/003/output.jsonl": "{\"record\":\"totals\",\"passed\":0,\"failed\":0,\"stopped\":0}"
  }).read_only
  console = Console.fake()
  assert main(Args.of(["logs"]), console, files) == 0
  assert console.printed_errors == []
  assert console.printed == [
    "code\terrors\tfirst_retry\twithin_three\tby_fix\tunfixed",
    "E0010\t4\t0\t0\t4\t0",
    "total\t4\t0\t0\t4\t0",
    "first_retry_rate\tn/a",
    "within_three_rate\tn/a",
    "tests_passed\t0/0 n/a"
  ]
end
