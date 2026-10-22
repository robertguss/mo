## Totals each column of a CSV table of numbers read from standard input.
module ColSum

## Why a line of the table was skipped.
enum Problem
  WrongCount(expected: Int, found: Int)
  NotANumber(field: Text)
end

fn to_text(problem: Problem) : Text
  case problem
    WrongCount(expected: e, found: f): "expected #{e} fields, found #{f}"
    NotANumber(field: field): "not a number: #{field}"
  end
end

## The numbers of one line, which must have `width` fields.
fn parse_row(line: Text, width: Int) : Result(List(Decimal), Problem)
  examples
    parse_row("1, 2.5", 2) == Ok([1, 2.5])
    parse_row("1", 2) == Error(WrongCount(expected: 2, found: 1))
    parse_row("1,x", 2) == Error(NotANumber(field: "x"))
  end

  fields = line.split(",").map(fn(field) field.trim end)
  return Error(WrongCount(expected: width, found: fields.size)) if fields.size != width
  var numbers = []
  for field in fields
    case field.to_decimal
      Some(n): numbers = numbers.append(n)
      None: return Error(NotANumber(field: field))
    end
  end
  Ok(numbers)
end

fn main(console: Console) : Int
  header = case console.read_line
    Some(line): line.split(",").map(fn(name) name.trim end)
    None:
      console.error("no header")
      return 1
  end
  var totals = header.map(fn(_) 0.0 end)
  var status = 0
  var number = 1
  while console.read_line is Some(line)
    continue if line.trim.empty?
    number += 1
    case parse_row(line, header.size)
      Ok(values): totals = totals.zip(values).map(fn(pair) pair.0 + pair.1 end)
      Error(problem):
        console.error("line #{number}: #{problem}")
        status = 1
    end
  end
  for (name, total) in header.zip(totals)
    console.print("#{name}: #{total.to_fixed(2)}")
  end
  status
end

test "columns are totalled, and a bad line is skipped"
  console = Console.fake_input(["a, b", "1, 2", "x, 3", "", "0.125,-1"])
  assert main(console) == 1
  assert console.printed == ["a: 1.13", "b: 1.00"]
  assert console.printed_errors == ["line 3: not a number: x"]
end
