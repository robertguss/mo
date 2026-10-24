## Counts compile-error episodes and the retries that resolve them.
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
  function_name: Text
end

struct Run
  command: Text
  errors: Set(ErrorKey)
  passed: Int
  tests: Int
end

struct Episode
  attempts: Int
  fix_seen?: Bool
end

struct Session
  counts: Map(Text, Counts)
  active: Map(ErrorKey, Episode)
  passed: Int
  tests: Int
  bad_json?: Bool
end

fn zero_counts() : Counts
  Counts(errors: 0, first_retry: 0, within_three: 0, by_fix: 0, unfixed: 0)
end

fn empty_session() : Session
  Session(counts: {}, active: {}, passed: 0, tests: 0, bad_json?: false)
end

fn advance(session: Session, run: Run) : Session
  var next = session
  if run.command == "fix"
    for (key, episode) in session.active
      next.active = next.active.set(key, Episode(attempts: episode.attempts, fix_seen?: true))
    end
    return next
  end

  for (key, episode) in session.active
    attempts = episode.attempts + 1
    if run.errors.has?(key)
      next.active = next.active.set(key, Episode(attempts: attempts, fix_seen?: false))
    else
      var counts = next.counts.get(key.code) or zero_counts()
      if episode.fix_seen?
        counts.by_fix += 1
      else
        if attempts == 1
          counts.first_retry += 1
        end
        if attempts <= 3
          counts.within_three += 1
        end
      end
      next.counts = next.counts.set(key.code, counts)
      next.active = next.active.remove(key)
    end
  end

  for key in run.errors
    if not session.active.has?(key)
      var counts = next.counts.get(key.code) or zero_counts()
      counts.errors += 1
      next.counts = next.counts.set(key.code, counts)
      next.active = next.active.set(key, Episode(attempts: 0, fix_seen?: false))
    end
  end
  if run.command == "test"
    next.passed = run.passed
    next.tests = run.tests
  end
  next
end

fn read_session(files: ReadFiles, console: Console, path: Text) : Result(Session, FileError)
  entries = try files.entries(path)
  var session = empty_session()
  for entry in entries.sort_by(fn(item) item.name end)
    continue if entry.kind != Folder
    continue if entry.name.char_count != 3 or not entry.name.chars.all?(fn(c) c.ascii_digit? end)
    run_path = "#{path}/#{entry.name}"
    command = (try files.read_text("#{run_path}/command")).trim
    if command == "fix"
      session = advance(session, Run(command: command, errors: Set.empty, passed: 0, tests: 0))
    else if command == "check" or command == "test"
      case try read_run(files, console, run_path, command)
        None: session.bad_json? = true
        Some(run): session = advance(session, run)
      end
    end
  end
  for (key, _) in session.active
    var counts = session.counts.get(key.code) or zero_counts()
    counts.unfixed += 1
    session.counts = session.counts.set(key.code, counts)
  end
  Ok(session)
end

fn add_counts(a: Counts, b: Counts) : Counts
  Counts(
    errors: a.errors + b.errors,
    first_retry: a.first_retry + b.first_retry,
    within_three: a.within_three + b.within_three,
    by_fix: a.by_fix + b.by_fix,
    unfixed: a.unfixed + b.unfixed
  )
end

fn tally(files: ReadFiles, console: Console, path: Text) : Result(Session, FileError)
  entries = try files.entries(path)
  var combined = empty_session()
  for entry in entries
    continue if entry.kind != Folder
    session = try read_session(files, console, "#{path}/#{entry.name}")
    for (code, counts) in session.counts
      combined.counts = combined.counts.set(
        code,
        add_counts(combined.counts.get(code) or zero_counts(), counts)
      )
    end
    combined.passed += session.passed
    combined.tests += session.tests
    combined.bad_json? = combined.bad_json? or session.bad_json?
  end
  Ok(combined)
end

fn field(value: Json, name: Text) : Json
  if value is Json.Object(fields)
    return fields.get(name) or Json.Null
  end
  Json.Null
end

fn string_value(value: Json) : Text
  if value is Json.String(text)
    return text
  end
  ""
end

fn int_value(value: Json) : Int
  if value is Json.Number(number)
    return number.to_int or 0
  end
  0
end

fn read_run(files: ReadFiles, console: Console, path: Text, command: Text) : Result(
  Option(Run),
  FileError
)
  output_path = "#{path}/output.jsonl"
  output = try files.read_text(output_path)
  var records = []
  var invalid? = false
  for (index, line) in output.lines.enumerate
    case Json.parse(line)
      Ok(record): records = records.append(record)
      Error(_):
        console.error("#{output_path}:#{index + 1}: not JSON")
        invalid? = true
    end
  end
  return Ok(None) if invalid?

  var errors = Set.empty
  var passed = 0
  var tests = 0
  for record in records
    kind = string_value(field(record, "record"))
    if kind == "diagnostic"
      primary = field(record, "primary")
      file = string_value(field(primary, "file"))
      source = try files.read_text("#{path}/src/#{file}")
      key = ErrorKey(
        code: string_value(field(record, "code")),
        file: file,
        function_name: function_at(source, int_value(field(primary, "line")))
      )
      errors = errors.add(key)
    else if kind == "totals"
      passed = int_value(field(record, "passed"))
      tests = passed + int_value(field(record, "failed")) + int_value(field(record, "stopped"))
    end
  end
  Ok(Some(Run(command: command, errors: errors, passed: passed, tests: tests)))
end

fn percentage(n: Int, denominator: Int) : Text
  examples
    percentage(1, 16) == "6.3%"
    percentage(0, 0) == "n/a"
    percentage(9, 10) == "90.0%"
  end

  return "n/a" if denominator == 0
  scaled = (n.to_big * 2000 + denominator.to_big) / (denominator.to_big * 2)
  "#{scaled / 10}.#{scaled % 10}%"
end

fn function_at(source: Text, line_number: Int) : Text
  examples
    function_at("module A\nfn alpha[T](x: T)\n  x\nend", 3) == "alpha"
    function_at("module A\n test \"indented\"\n", 2) == "(module)"
    function_at("test \"a scenario\"  \n  assert true", 2) == "test \"a scenario\""
  end

  var found = "(module)"
  for line in source.lines.take(line_number.max(0))
    if line.starts_with?("fn ")
      after = line.drop_prefix("fn ") or ""
      found = after.split("(").first or ""
      found = found.split("[").first or ""
      found = found.trim
    else if line.starts_with?("test ")
      found = line.trim
    end
  end
  found
end

fn print_counts(console: Console, code: Text, counts: Counts)
  console.print(
    "#{code}\t#{counts.errors}\t#{counts.first_retry}\t#{counts.within_three}\t#{counts.by_fix}\t#{counts
      .unfixed}"
  )
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
  console.print("code\terrors\tfirst_retry\twithin_three\tby_fix\tunfixed")
  var total = zero_counts()
  for code in summary.counts.keys.sort
    counts = summary.counts.get(code) or zero_counts()
    print_counts(console, code, counts)
    total = add_counts(total, counts)
  end
  print_counts(console, "total", total)
  denominator = total.errors - total.by_fix
  console.print("first_retry_rate\t#{percentage(total.first_retry, denominator)}")
  console.print("within_three_rate\t#{percentage(total.within_three, denominator)}")
  console.print(
    "tests_passed\t#{summary.passed}/#{summary.tests} #{percentage(summary.passed, summary.tests)}"
  )
  if summary.bad_json?: 1 else: 0
end

test "the sample table deduplicates diagnostics and follows functions across moved lines"
  first = "{\"record\":\"diagnostic\",\"code\":\"E0050\",\"primary\":{\"file\":\"a.mo\",\"line\":2}}"
  second = "{\"record\":\"diagnostic\",\"code\":\"E0078\",\"primary\":{\"file\":\"a.mo\",\"line\":4}}"
  moved = "{\"record\":\"diagnostic\",\"code\":\"E0078\",\"primary\":{\"file\":\"a.mo\",\"line\":5}}"
  files = Files.in_memory({
    "logs/s/003/command": "test\n",
    "logs/s/003/output.jsonl": "{\"record\":\"totals\",\"passed\":3,\"failed\":1,\"stopped\":0}\n",
    "logs/s/002/command": "check\n",
    "logs/s/002/output.jsonl": moved,
    "logs/s/002/src/a.mo": "module A\nfn f()\nend\n\nfn g()\nend",
    "logs/s/001/command": "check\n",
    "logs/s/001/output.jsonl": "#{first}\n#{second}\n#{first}\n{\"record\":\"future\",\"extra\":[1,true]}\n",
    "logs/s/001/src/a.mo": "module A\nfn f()\nend\nfn g()\nend"
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

test "usage and unreadable logs have status two without a table"
  files = Files.in_memory({"not-a-folder": "text"}).read_only
  missing_args = Console.fake()
  assert main(Args.of([]), missing_args, files) == 2
  assert missing_args.printed == []
  assert missing_args.printed_errors == ["usage: tally LOGS"]
  extra_args = Console.fake()
  assert main(Args.of(["a", "b"]), extra_args, files) == 2
  assert extra_args.printed == []
  assert extra_args.printed_errors == ["usage: tally LOGS"]
  missing_folder = Console.fake()
  assert main(Args.of(["missing"]), missing_folder, files) == 2
  assert missing_folder.printed == []
  assert missing_folder.printed_errors == ["missing: no such file"]
  not_folder = Console.fake()
  assert main(Args.of(["not-a-folder"]), not_folder, files) == 2
  assert not_folder.printed == []
  assert not_folder.printed_errors == ["not-a-folder: not a folder"]
end

test "episodes recur, retries have boundaries, and only the last report can give fix credit"
  f = "{\"record\":\"diagnostic\",\"code\":\"E0001\",\"primary\":{\"file\":\"a.mo\",\"line\":2}}"
  g = "{\"record\":\"diagnostic\",\"code\":\"E0001\",\"primary\":{\"file\":\"a.mo\",\"line\":4}}"
  files = Files.in_memory({
    "logs/b/001/command": "check",
    "logs/b/001/output.jsonl": "{\"record\":\"diagnostic\",\"code\":\"E0002\",\"primary\":{\"file\":\"a.mo\",\"line\":1}}\n{\"record\":\"diagnostic\",\"code\":\"E0002\",\"primary\":{\"file\":\"a.mo\",\"line\":3}}\n{\"record\":\"diagnostic\",\"code\":\"E0002\",\"primary\":{\"file\":\"b.mo\",\"line\":1}}",
    "logs/b/001/src/a.mo": "module A\ntest \"a test\"\n  assert true\nend",
    "logs/b/001/src/b.mo": "module B",
    "logs/b/002/command": "test",
    "logs/b/002/output.jsonl": "{\"record\":\"totals\",\"passed\":1,\"failed\":0,\"stopped\":2}",
    "logs/ignored-file": "ignore",
    "logs/a/not-a-run/notes": "ignore"
  })
  for number in 1..16
    padded = number.to_text.pad_left(3).replace(" ", "0")
    path = "logs/a/#{padded}"
    command = case number
      2 | 12 | 15: "fix"
      8 | 13: "test"
      _: "check"
    end
    diagnostics = case number
      1: "#{f}\n#{g}"
      3 | 4 | 6 | 7 | 8 | 9 | 11 | 14: f
      _: "{\"record\":\"version\",\"format\":\"1.0\"}"
    end
    totals = if number == 8
      "{\"record\":\"totals\",\"passed\":9,\"failed\":1,\"stopped\":1}"
    else
      "{\"record\":\"totals\",\"passed\":2,\"failed\":1,\"stopped\":1}"
    end
    assert files.make_folder("#{path}/src") == Ok(Unit)
    assert files.write_text("#{path}/command", command) == Ok(Unit)
    assert files.write_text("#{path}/src/a.mo", "module A\nfn f()\nend\nfn g()\nend") == Ok(Unit)
    if command != "fix"
      assert files.write_text("#{path}/output.jsonl", "#{diagnostics}\n#{totals}") == Ok(Unit)
    end
  end
  console = Console.fake()
  assert main(Args.of(["logs"]), console, files.read_only) == 0
  assert console.printed_errors == []
  assert console.printed == [
    "code\terrors\tfirst_retry\twithin_three\tby_fix\tunfixed",
    "E0001\t5\t0\t1\t2\t1",
    "E0002\t3\t3\t3\t0\t0",
    "total\t8\t3\t4\t2\t1",
    "first_retry_rate\t50.0%",
    "within_three_rate\t66.7%",
    "tests_passed\t3/7 42.9%"
  ]
end

test "invalid JSON skips whole runs, preserves fix markers and retains the last valid test"
  diagnostic = "{\"record\":\"diagnostic\",\"code\":\"E0001\",\"primary\":{\"file\":\"a.mo\",\"line\":1}}"
  files = Files.in_memory({
    "logs/s/001/command": "test",
    "logs/s/001/output.jsonl": "#{diagnostic}\n{\"record\":\"totals\",\"passed\":4,\"failed\":1,\"stopped\":0}",
    "logs/s/001/src/a.mo": "module A",
    "logs/s/002/command": "fix",
    "logs/s/003/command": "test",
    "logs/s/003/output.jsonl": "#{diagnostic}\r\nnot JSON\r\n\r\n{\"record\":\"totals\",\"passed\":99}\n",
    "logs/s/004/command": "check",
    "logs/s/004/output.jsonl": "{\"record\":\"new-record\",\"passed\":1000}\n42\nnull\n",
    "logs/s/005/command": "test",
    "logs/s/005/output.jsonl": "{",
    "logs/s/006/command": "check",
    "logs/s/006/output.jsonl": diagnostic,
    "logs/s/006/src/a.mo": "module A"
  }).read_only
  console = Console.fake()
  assert main(Args.of(["logs"]), console, files) == 1
  assert console.printed_errors == [
    "logs/s/003/output.jsonl:2: not JSON",
    "logs/s/003/output.jsonl:3: not JSON",
    "logs/s/005/output.jsonl:1: not JSON"
  ]
  assert console.printed == [
    "code\terrors\tfirst_retry\twithin_three\tby_fix\tunfixed",
    "E0001\t2\t0\t0\t1\t1",
    "total\t2\t0\t0\t1\t1",
    "first_retry_rate\t0.0%",
    "within_three_rate\t0.0%",
    "tests_passed\t4/5 80.0%"
  ]
end
