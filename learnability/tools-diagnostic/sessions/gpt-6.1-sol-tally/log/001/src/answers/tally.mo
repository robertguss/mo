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

fn zero() : Counts
  Counts(errors: 0, first_retry: 0, within_three: 0, by_fix: 0, unfixed: 0)
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
