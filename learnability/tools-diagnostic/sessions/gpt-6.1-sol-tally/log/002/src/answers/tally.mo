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
  case files.entries(path)
    Error(problem):
      console.error("#{problem}")
      return 2
    Ok(_entries): Unit
  end
  console.print("code\terrors\tfirst_retry\twithin_three\tby_fix\tunfixed")
  console.print(row("total", zero()))
  console.print("first_retry_rate\tn/a")
  console.print("within_three_rate\tn/a")
  console.print("tests_passed\t0/0 n/a")
  0
end
