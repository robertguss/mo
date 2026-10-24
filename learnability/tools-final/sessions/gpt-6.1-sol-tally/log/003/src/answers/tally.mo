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

fn read_run(files: ReadFiles, console: Console, path: Text, command: Text) : Result(Option(Run), FileError)
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
  case files.entries(path)
    Error(problem):
      console.error("#{problem}")
      return 2
    Ok(_): Unit
  end
  console.print("code\terrors\tfirst_retry\twithin_three\tby_fix\tunfixed")
  print_counts(console, "total", zero_counts())
  console.print("first_retry_rate\tn/a")
  console.print("within_three_rate\tn/a")
  console.print("tests_passed\t0/0 n/a")
  0
end
