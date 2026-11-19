## Reads JSON from standard input and prints it laid out with two-space
## indentation; or, given a path such as items.0.name, prints only the value at
## that path. A number prints as it was written, so 1.50 stays 1.50.
##
##   json [PATH] < FILE.json
module JsonQuery
use Json

fn read_input(console: Console) : Text
  var lines = []
  while console.read_line is Some(line)
    lines = lines.append(line)
  end
  lines.join("\n")
end

fn main(args: Args, console: Console) : Int
  path = case args.list
    []: ""
    [steps]: steps
    _:
      console.error("usage: json [PATH] < FILE.json")
      return 2
  end
  case Json.parse(read_input(console))
    Error(e):
      console.error("json: line #{e.line}, column #{e.column}: #{e.problem}")
      1
    Ok(value): show(value.path(path), console)
  end
end

fn show(found: Option(Json), console: Console) : Int
  case found
    Some(value):
      console.print(value.pretty)
      0
    None:
      console.error("json: nothing is at that path")
      1
  end
end

test "a path picks out one value, and no path prints the whole document"
  console = Console.fake_input(["{\"items\": [{\"name\": \"pen\"}]}"])
  assert main(Args.of(["items.0.name"]), console) == 0
  assert console.printed == ["\"pen\""]
  whole = Console.fake_input(["[1.50,", "{}]"])
  assert main(Args.of([]), whole) == 0
  assert whole.printed == ["[\n  1.50,\n  {}\n]"]
end

test "a usage mistake, a bad document and a missing path each fail"
  usage = Console.fake_input([])
  assert main(Args.of(["a", "b"]), usage) == 2
  assert usage.printed_errors == ["usage: json [PATH] < FILE.json"]
  broken = Console.fake_input(["[1,"])
  assert main(Args.of([]), broken) == 1
  assert broken.printed_errors == ["json: line 1, column 4: UnexpectedEnd"]
  missing = Console.fake_input(["[1]"])
  assert main(Args.of(["3"]), missing) == 1
  assert missing.printed_errors == ["json: nothing is at that path"]
end
