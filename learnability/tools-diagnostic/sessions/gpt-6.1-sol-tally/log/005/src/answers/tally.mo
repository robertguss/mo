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
