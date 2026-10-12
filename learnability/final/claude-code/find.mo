## find: prints the lines of files that contain a phrase, with each file's name
## and the line's number, as "FILE:NUMBER:LINE". Lines count from 1.
##
##   find [--ignore-case] [--count] PHRASE FILE...
##
## --ignore-case (-i) ignores ASCII case. --count (-c) prints only how many
## lines matched in each file, as "FILE:COUNT". Short options may be grouped,
## as in -ic, and "--" ends the options, for a PHRASE or FILE that starts
## with "-".
##
## The exit status is 0 when a line matched, 1 when none did, and 2 on a usage
## error or a file that can't be read.
module Find

## Printed on standard error after a usage error.
usage = "usage: find [--ignore-case | -i] [--count | -c] PHRASE FILE..."

## What the command line asks for.
struct Request
  phrase: Text
  paths: List(Text)
  ignore_case?: Bool
  count_only?: Bool
end

## A switch that an option turns on.
enum Flag
  IgnoreCase
  CountOnly
end

## What can be wrong with a command line.
enum UsageError
  UnknownOption(Text)
  MissingPhrase
  MissingFile
end

## A usage error as it is reported, after "find: ".
fn to_text(mistake: UsageError) : Text
  case mistake
    UnknownOption(option): "unknown option: #{option}"
    MissingPhrase: "no PHRASE given"
    MissingFile: "no FILE given"
  end
end

## Searches each file named, in order, and gives the exit status: 0 when a line
## matched, 1 when none did, and 2 on a usage error or a file that can't be
## read. A file that can't be read is reported on standard error, and the
## other files are still searched.
fn main(args: Args, console: Console, files: ReadFiles) : Int
  request = case parse_args(args.list)
    Ok(parsed): parsed
    Error(mistake):
      console.error("find: #{mistake}")
      console.error(usage)
      return 2
  end
  var matched? = false
  var unreadable? = false
  for path in request.paths
    case files.read_text(path)
      Ok(text):
        found = matching_lines(text, request.phrase, request.ignore_case?)
        if request.count_only?
          console.print("#{path}:#{found.size}")
        else
          for (number, line) in found
            console.print("#{path}:#{number}:#{line}")
          end
        end
        matched? = matched? or not found.empty?
      Error(failure):
        console.error("find: #{failure}")
        unreadable? = true
    end
  end
  if unreadable?: 2 else if matched?: 0 else: 1
end

## Reads the command line. Options may come anywhere before "--"; every other
## argument, and every argument after "--", is PHRASE and then the files. A lone
## "-" is not an option.
fn parse_args(args: List(Text)) : Result(Request, UsageError)
  examples
    parse_args(["-i", "--count", "pear", "a.txt", "b.txt"]) == Ok(Request(
      phrase: "pear",
      paths: ["a.txt", "b.txt"],
      ignore_case?: true,
      count_only?: true
    ))
    parse_args(["pear", "a.txt", "-c"]) == Ok(Request(
      phrase: "pear",
      paths: ["a.txt"],
      ignore_case?: false,
      count_only?: true
    ))
    parse_args(["--", "-i", "a.txt"]) == Ok(Request(
      phrase: "-i",
      paths: ["a.txt"],
      ignore_case?: false,
      count_only?: false
    ))
    parse_args(["pear", "--", "-c", "-"]) == Ok(Request(
      phrase: "pear",
      paths: ["-c", "-"],
      ignore_case?: false,
      count_only?: false
    ))
    parse_args([]) == Error(MissingPhrase)
    parse_args(["-c"]) == Error(MissingPhrase)
    parse_args(["pear"]) == Error(MissingFile)
    parse_args(["--colour", "pear", "a.txt"]) == Error(UnknownOption("--colour"))
  end

  var ignore_case? = false
  var count_only? = false
  var options_done? = false
  var operands = []
  for arg in args
    if options_done? or arg == "-" or not arg.starts_with?("-")
      operands = operands.append(arg)
    else if arg == "--"
      options_done? = true
    else
      case flags(arg)
        Some(turned_on):
          ignore_case? = ignore_case? or turned_on.contains?(IgnoreCase)
          count_only? = count_only? or turned_on.contains?(CountOnly)
        None: return Error(UnknownOption(arg))
      end
    end
  end
  case operands
    []: Error(MissingPhrase)
    [_]: Error(MissingFile)
    [phrase, ..paths]:
      Ok(Request(
        phrase: phrase,
        paths: paths,
        ignore_case?: ignore_case?,
        count_only?: count_only?
      ))
  end
end

## The switches an option turns on, or None when it isn't one of find's
## options. Short options may be grouped after one "-", as in "-ic".
fn flags(option: Text) : Option(List(Flag))
  examples
    flags("--ignore-case") == Some([IgnoreCase])
    flags("--count") == Some([CountOnly])
    flags("-i") == Some([IgnoreCase])
    flags("-ci") == Some([CountOnly, IgnoreCase])
    flags("-x") == None
    flags("-ix") == None
    flags("--colour") == None
  end

  case option
    "--ignore-case": Some([IgnoreCase])
    "--count": Some([CountOnly])
    _:
      letters = (option.drop_prefix("-") or "").chars
      known = letters.filter_map(short_flag)
      if letters.empty? or known.size != letters.size: None else: Some(known)
  end
end

## The switch a short option's letter stands for.
fn short_flag(letter: Char) : Option(Flag)
  case letter
    'i': Some(IgnoreCase)
    'c': Some(CountOnly)
    _: None
  end
end

## The lines of a text that contain the phrase, each with its number, counting
## from 1. With ignore_case?, ASCII letters match in either case.
fn matching_lines(text: Text, phrase: Text, ignore_case?: Bool) : List((Int, Text))
  examples
    matching_lines("one\ntwo\nthree\n", "o", false) == [(1, "one"), (2, "two")]
    matching_lines("red apple\r\ngreen pear\r\nApple pie", "apple", false) == [(1, "red apple")]
    matching_lines("red apple\ngreen pear\nApple pie", "APPLE", true) == [
      (1, "red apple"),
      (3, "Apple pie")
    ]
    matching_lines("red apple\n", "plum", true) == []
    matching_lines("", "apple", false) == []
  end

  needle = folded(phrase, ignore_case?)
  text.lines
    .enumerate
    .filter(fn(numbered) folded(numbered.1, ignore_case?).contains?(needle) end)
    .map(fn(numbered) (numbered.0 + 1, numbered.1) end)
end

## A text as it is compared: in ASCII lowercase when case is ignored.
fn folded(text: Text, ignore_case?: Bool) : Text
  examples
    folded("Straße ÄPFEL", true) == "straße Äpfel"
    folded("Apple", false) == "Apple"
  end

  if ignore_case?: text.ascii_lowercase else: text
end

test "matching lines are printed with the file's name and the line's number"
  console = Console.fake()
  files = Files.in_memory({"fruit.txt": "red apple\ngreen pear\nApple pie\n"}).read_only
  assert main(Args.of(["apple", "fruit.txt"]), console, files) == 0
  assert console.printed == ["fruit.txt:1:red apple"]
  assert console.printed_errors == []
end

test "--ignore-case matches ASCII letters in either case, in every file"
  console = Console.fake()
  files = Files.in_memory({
    "fruit.txt": "red apple\ngreen pear\nApple pie\n",
    "more.txt": "PINEAPPLE\nplum\n"
  }).read_only
  assert main(Args.of(["-i", "apple", "fruit.txt", "more.txt"]), console, files) == 0
  assert console.printed == [
    "fruit.txt:1:red apple",
    "fruit.txt:3:Apple pie",
    "more.txt:1:PINEAPPLE"
  ]
end

test "--count prints only how many lines matched in each file"
  console = Console.fake()
  files = Files.in_memory({
    "fruit.txt": "red apple\ngreen pear\nApple pie\n",
    "more.txt": "plum\n"
  }).read_only
  args = Args.of(["--count", "--ignore-case", "apple", "fruit.txt", "more.txt"])
  assert main(args, console, files) == 0
  assert main(Args.of(["apple", "fruit.txt", "-ic"]), console, files) == 0
  assert console.printed == ["fruit.txt:2", "more.txt:0", "fruit.txt:2"]
end

test "the status is 1 when no line matched"
  console = Console.fake()
  files = Files.in_memory({"fruit.txt": "red apple\n", "more.txt": ""}).read_only
  assert main(Args.of(["plum", "fruit.txt", "more.txt"]), console, files) == 1
  assert main(Args.of(["-c", "plum", "fruit.txt"]), console, files) == 1
  assert console.printed == ["fruit.txt:0"]
end

test "a file that can't be read gives status 2, and the other files are still searched"
  console = Console.fake()
  files = Files.in_memory({"fruit.txt": "red apple\n"}).read_only
  assert main(Args.of(["apple", "missing.txt", "fruit.txt"]), console, files) == 2
  assert console.printed == ["fruit.txt:1:red apple"]
  assert console.printed_errors == ["find: missing.txt: no such file"]
end

test "a usage error gives status 2 and prints the usage"
  console = Console.fake()
  files = Files.in_memory({"fruit.txt": "red apple\n"}).read_only
  assert main(Args.of(["--colour", "apple", "fruit.txt"]), console, files) == 2
  assert main(Args.of(["apple"]), console, files) == 2
  assert main(Args.of([]), console, files) == 2
  assert console.printed == []
  assert console.printed_errors == [
    "find: unknown option: --colour",
    usage,
    "find: no FILE given",
    usage,
    "find: no PHRASE given",
    usage
  ]
end

test "a phrase that starts with - is given after --"
  console = Console.fake()
  files = Files.in_memory({"notes.txt": "a -i flag\nno flags\n"}).read_only
  assert main(Args.of(["--", "-i", "notes.txt"]), console, files) == 0
  assert console.printed == ["notes.txt:1:a -i flag"]
end
