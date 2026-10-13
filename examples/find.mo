## Prints the lines of files that contain a phrase, with each file's name and
## the line's number.
##
##   find [--ignore-case] [--count] PHRASE FILE...
##
## --ignore-case (-i) ignores the difference between ASCII capitals and small
## letters; --count (-c) prints only how many lines matched in each file. As
## with grep, the exit status is 0 when a line matched, 1 when none did, and 2
## on a usage error or a file that can't be read.
module Find

struct Search
  phrase: Text
  files: List(Text)
  ignore_case?: Bool
  count_only?: Bool
end

enum UsageError
  NoPhrase
  NoFiles
  UnknownOption(option: Text)
end

fn to_text(problem: UsageError) : Text
  case problem
    NoPhrase: "no phrase to find"
    NoFiles: "no files to search"
    UnknownOption(option: o): "unknown option #{o}"
  end
end

fn parse_args(args: List(Text)) : Result(Search, UsageError)
  examples
    parse_args(["cat", "a.txt"]) == Ok(search_for("cat", ["a.txt"]))
    parse_args(["-i", "--count", "cat", "a.txt"]) is
      Ok(Search(ignore_case?: true, count_only?: true))
    parse_args(["--fast", "cat", "a.txt"]) == Error(UnknownOption(option: "--fast"))
    parse_args(["cat"]) == Error(NoFiles)
    parse_args([]) == Error(NoPhrase)
  end

  var search = search_for("", [])
  var rest = []
  for arg in args
    case arg
      "--ignore-case" | "-i": search.ignore_case? = true
      "--count" | "-c": search.count_only? = true
      _ if arg.starts_with?("-"): return Error(UnknownOption(option: arg))
      _: rest = rest.append(arg)
    end
  end
  case rest
    []: Error(NoPhrase)
    [_]: Error(NoFiles)
    [phrase, ..files]:
      search.phrase = phrase
      search.files = files
      Ok(search)
  end
end

fn search_for(phrase: Text, files: List(Text)) : Search
  Search(phrase: phrase, files: files, ignore_case?: false, count_only?: false)
end

## The lines of text that contain the phrase, each with its number, from 1.
fn matches(text: Text, search: Search) : List((Int, Text))
  examples
    matches("a cat\nno\nCat!", search_for("cat", [])) == [(1, "a cat")]
    matches("", search_for("cat", [])) == []
  end

  phrase = folded(search.phrase, search.ignore_case?)
  var found = []
  for (index, line) in text.lines.enumerate
    if folded(line, search.ignore_case?).contains?(phrase)
      found = found.append((index + 1, line))
    end
  end
  found
end

fn folded(text: Text, ignore_case?: Bool) : Text
  if ignore_case?: text.ascii_lowercase else: text
end

fn main(args: Args, console: Console, files: ReadFiles) : Int
  case parse_args(args.list)
    Ok(search): run(search, console, files)
    Error(problem):
      console.error("find: #{problem}")
      console.error("usage: find [--ignore-case] [--count] PHRASE FILE...")
      2
  end
end

fn run(search: Search, console: Console, files: ReadFiles) : Int
  var matched? = false
  var failed? = false
  for name in search.files
    case files.read_text(name)
      Error(failure):
        console.error("find: #{failure}")
        failed? = true
      Ok(text):
        found = matches(text, search)
        matched? = matched? or not found.empty?
        if search.count_only?
          console.print("#{name}: #{found.size}")
        else
          for (number, line) in found
            console.print("#{name}:#{number}: #{line}")
          end
        end
    end
  end
  if failed?: 2 else if matched?: 0 else: 1
end

test "matching ignores case only when asked"
  files = Files.in_memory({"pets.txt": "a cat\nCats!\ndog\n"}).read_only
  console = Console.fake()
  assert main(Args.of(["-i", "cat", "pets.txt"]), console, files) == 0
  assert console.printed == ["pets.txt:1: a cat", "pets.txt:2: Cats!"]
end

test "no match gives status 1, and a missing file status 2"
  files = Files.in_memory({"pets.txt": "dog\n"}).read_only
  assert main(Args.of(["cat", "pets.txt"]), Console.fake(), files) == 1
  assert main(Args.of(["cat", "gone.txt"]), Console.fake(), files) == 2
end
