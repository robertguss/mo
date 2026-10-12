## Prints the lines of files that contain a phrase, with each file's name and
## the line's number.
##
## Usage: find [--ignore-case] [--count] PHRASE FILE...
##
## --ignore-case (-i) ignores ASCII case, and --count (-c) prints only how many
## lines matched in each file. Options come before PHRASE; short ones combine,
## as in -ic, and "--" ends them, so that a phrase may start with "-". A
## matching line prints as FILE:NUMBER:LINE, counting lines from 1, and a count
## prints as FILE:COUNT. The exit status is 0 when a line matched, 1 when none
## did, and 2 on a usage error or a file that can't be read. The files after
## one that can't be read are still searched.
module Find

## Printed to standard error after a usage error.
usage = "Usage: find [--ignore-case] [--count] PHRASE FILE..."

## What the command line asks for.
struct Request
  ignore_case?: Bool
  count?: Bool
  phrase: Text
  paths: List(Text)
end

## What can be wrong with the command line.
enum UsageError
  UnknownOption(Text)
  MissingPhrase
  MissingFile
end

## How a usage error reads.
fn to_text(problem: UsageError) : Text
  case problem
    UnknownOption(word): "unknown option #{word}"
    MissingPhrase: "no PHRASE given"
    MissingFile: "no FILE given"
  end
end

## Reads the command line, then searches the files it names.
fn main(args: Args, console: Console, files: ReadFiles) : Int
  case parse(args.list)
    Ok(request): search(request, console, files)
    Error(problem):
      console.error("find: #{problem}")
      console.error(usage)
      2
  end
end

## Reads the options up to PHRASE or "--", then PHRASE and the files.
fn parse(words: List(Text)) : Result(Request, UsageError)
  var ignore_case? = false
  var count? = false
  var rest = words
  while rest is [word, ..others] and word.starts_with?("-") and word != "-"
    rest = others
    break if word == "--"
    case word
      "--ignore-case": ignore_case? = true
      "--count": count? = true
      _ if word.starts_with?("--"): return Error(UnknownOption(word))
      _:
        for flag in word.chars.drop(1)
          case flag
            'i': ignore_case? = true
            'c': count? = true
            _: return Error(UnknownOption(word))
          end
        end
    end
  end
  case rest
    []: Error(MissingPhrase)
    [_]: Error(MissingFile)
    [phrase, ..paths]:
      Ok(Request(
        ignore_case?: ignore_case?,
        count?: count?,
        phrase: phrase,
        paths: paths
      ))
  end
end

## Searches each file in turn and prints what it finds. Gives the exit status.
fn search(request: Request, console: Console, files: ReadFiles) : Int
  var total = 0
  var failed? = false
  for path in request.paths
    case files.read_text(path)
      Ok(text):
        found = matches(text, request.phrase, request.ignore_case?)
        if request.count?
          console.print("#{path}:#{found.size}")
        else
          for (number, line) in found
            console.print("#{path}:#{number}:#{line}")
          end
        end
        total += found.size
      Error(problem):
        console.error("find: #{problem}")
        failed? = true
    end
  end
  if failed?: 2 else if total > 0: 0 else: 1
end

## The lines of a text that contain the phrase, each with its number, counting
## from 1.
fn matches(text: Text, phrase: Text, ignore_case?: Bool) : List((Int, Text))
  examples
    matches("one\ntwo\nthree\n", "o", false) == [(1, "one"), (2, "two")]
    matches("Red\nred\nRED", "red", false) == [(2, "red")]
    matches("Red\nred\nRED", "rEd", true) == [(1, "Red"), (2, "red"), (3, "RED")]
    matches("", "a", false) == []
  end

  needle = fold(phrase, ignore_case?)
  text.lines.enumerate
    .filter(fn(entry) fold(entry.1, ignore_case?).contains?(needle) end)
    .map(fn(entry) (entry.0 + 1, entry.1) end)
end

## A text as it is compared: with its ASCII letters in lowercase when case is
## ignored, and unchanged otherwise.
fn fold(text: Text, ignore_case?: Bool) : Text
  examples
    fold("ÉCOLE", true) == "École"
    fold("ÉCOLE", false) == "ÉCOLE"
  end

  if ignore_case?: text.ascii_lowercase else: text
end

test "matching lines print with the file's name and the line's number"
  console = Console.fake()
  files = Files.in_memory({"notes.txt": "one\ntwo\nthree\n"}).read_only
  assert main(Args.of(["o", "notes.txt"]), console, files) == 0
  assert console.printed == ["notes.txt:1:one", "notes.txt:2:two"]
end

test "--count prints how many lines matched in each file"
  console = Console.fake()
  files = Files.in_memory({"a.txt": "Cat\ndog\ncat\n", "b.txt": "bird\n"}).read_only
  assert main(Args.of(["--count", "--ignore-case", "CAT", "a.txt", "b.txt"]), console, files) == 0
  assert console.printed == ["a.txt:2", "b.txt:0"]
end

test "short options combine, and -- lets a phrase start with a dash"
  console = Console.fake()
  files = Files.in_memory({"a.txt": "-x\nA -X\nx\n"}).read_only
  assert main(Args.of(["-ic", "--", "-x", "a.txt"]), console, files) == 0
  assert console.printed == ["a.txt:2"]
end

test "the exit status is 1 when no line matched"
  console = Console.fake()
  files = Files.in_memory({"a.txt": "one\ntwo\n"}).read_only
  assert main(Args.of(["One", "a.txt"]), console, files) == 1
  assert console.printed == []
end

test "a file that can't be read gives status 2, and the other files are still searched"
  console = Console.fake()
  files = Files.in_memory({"a.txt": "one\n"}).read_only
  assert main(Args.of(["one", "missing.txt", "a.txt"]), console, files) == 2
  assert console.printed == ["a.txt:1:one"]
  assert console.printed_errors == ["find: missing.txt: no such file"]
end

test "a usage error prints the usage, and gives status 2"
  console = Console.fake()
  files = Files.in_memory({"a.txt": "one\n"}).read_only
  assert main(Args.of(["one"]), console, files) == 2
  assert main(Args.of(["--color", "one", "a.txt"]), console, files) == 2
  assert console.printed == []
  assert console.printed_errors == [
    "find: no FILE given",
    usage,
    "find: unknown option --color",
    usage
  ]
end

test "options end at the phrase, or at --"
  assert parse(["-i", "--count", "x", "a.txt"]) ==
    Ok(Request(ignore_case?: true, count?: true, phrase: "x", paths: ["a.txt"]))
  assert parse(["x", "-c", "a.txt"]) ==
    Ok(Request(ignore_case?: false, count?: false, phrase: "x", paths: ["-c", "a.txt"]))
  assert parse(["--", "--count", "a.txt"]) ==
    Ok(Request(ignore_case?: false, count?: false, phrase: "--count", paths: ["a.txt"]))
  assert parse(["-", "a.txt"]) ==
    Ok(Request(ignore_case?: false, count?: false, phrase: "-", paths: ["a.txt"]))
end

test "the command line needs known options, a phrase and a file"
  assert parse([]) == Error(MissingPhrase)
  assert parse(["-c"]) == Error(MissingPhrase)
  assert parse(["x"]) == Error(MissingFile)
  assert parse(["-iv", "x", "a.txt"]) == Error(UnknownOption("-iv"))
  assert parse(["--ignore", "x", "a.txt"]) == Error(UnknownOption("--ignore"))
end
