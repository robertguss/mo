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

fn zero_counts() : Counts
  Counts(errors: 0, first_retry: 0, within_three: 0, by_fix: 0, unfixed: 0)
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
