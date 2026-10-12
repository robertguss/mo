## word-freq: counts how often each word appears in the files named on the
## command line, ignoring ASCII case, and prints the most frequent words as
## `count word`: the most frequent first, and equally frequent words in
## alphabetical order. A word is a run of ASCII letters and apostrophes, and
## is counted and printed in ASCII lowercase.
##
##   word-freq [--top N] [FILE...]
##
## It prints 10 words, or N with `--top N` (or `--top=N`). With no files
## named, or for the name `-`, it reads standard input. A file that can't be
## read is reported on standard error, the others are still counted, and the
## exit status is then 1. A mistake on the command line is reported with the
## usage, and the exit status is then 2.
module WordFreq

usage = "usage: word-freq [--top N] [FILE...]"

default_top = 10

## What the command line asks for: how many words to print, and the names of
## the files to read them from.
struct Request
  top: Int
  names: List(Text)
end

## Counts the words in every source, prints the most frequent, and returns the
## exit status.
fn main(args: Args, console: Console, files: ReadFiles) : Int
  request = case parse_args(args.list, default_top, [])
    Ok(parsed): parsed
    Error(problem):
      console.error("word-freq: #{problem}")
      console.error(usage)
      return 2
  end
  sources = if request.names.empty?: ["-"] else: request.names
  var counts = {}
  var failed? = false
  for name in sources
    case read_source(name, console, files)
      Ok(text):
        for word in words_in(text)
          counts = counts.update(word, 0, fn(so_far) so_far + 1 end)
        end
      Error(failure):
        failed? = true
        console.error("word-freq: #{failure}")
    end
  end
  for (word, count) in most_frequent(counts, request.top)
    console.print("#{count} #{word}")
  end
  if failed?: 1 else: 0
end

## Reads the command line: `--top N` or `--top=N` anywhere, `--` before names
## that start with `-`, and the names of the files.
fn parse_args(remaining: List(Text), top: Int, names: List(Text)) : Result(Request, Text)
  examples
    parse_args([], 10, []) == Ok(Request(top: 10, names: []))
    parse_args(["a.txt", "--top", "3", "b.txt"], 10, []) == Ok(Request(top: 3, names: ["a.txt", "b.txt"]))
    parse_args(["--top=0", "--", "--top", "-"], 10, []) == Ok(Request(top: 0, names: ["--top", "-"]))
    parse_args(["-"], 10, []) == Ok(Request(top: 10, names: ["-"]))
    parse_args(["a.txt", "--top"], 10, []) == Error("--top needs a number")
    parse_args(["--top", "-2"], 10, []) == Error("--top needs a whole number of 0 or more, not \"-2\"")
    parse_args(["--tpo", "3"], 10, []) == Error("unknown option --tpo")
  end

  case remaining
    []: Ok(Request(top: top, names: names))
    ["--", ..rest]: Ok(Request(top: top, names: names.concat(rest)))
    ["--top"]: Error("--top needs a number")
    ["--top", value, ..rest]: parse_args(rest, try parse_top(value), names)
    [arg, ..rest]:
      case arg.drop_prefix("--top=")
        Some(value): parse_args(rest, try parse_top(value), names)
        None if arg.starts_with?("-") and arg != "-": Error("unknown option #{arg}")
        None: parse_args(rest, top, names.append(arg))
      end
  end
end

## The number given with `--top`: a whole number, 0 or more.
fn parse_top(value: Text) : Result(Int, Text)
  examples
    parse_top("3") == Ok(3)
    parse_top("0") == Ok(0)
    parse_top("ten") == Error("--top needs a whole number of 0 or more, not \"ten\"")
  end

  case value.to_int
    Some(top) if top >= 0: Ok(top)
    Some(_) | None: Error("--top needs a whole number of 0 or more, not \"#{value}\"")
  end
end

## The text of the named file, or of standard input for `-`.
fn read_source(name: Text, console: Console, files: ReadFiles) : Result(Text, FileError)
  return Ok(read_all(console)) if name == "-"
  data = try files.read_bytes(name)
  Ok(data.decode_utf8 or latin1_text(data))
end

## Everything on standard input.
fn read_all(console: Console) : Text
  var lines = []
  while console.read_line is Some(line)
    lines = lines.append(line)
  end
  lines.join("\n")
end

## Reads each byte as the character with its code, as Latin-1 does. For a file
## that isn't UTF-8, this keeps every ASCII character, and so every word.
fn latin1_text(data: Bytes) : Text
  examples
    latin1_text("Don't".bytes) == "Don't"
    latin1_text("é!".bytes) == "\u{C3}\u{A9}!"
    latin1_text("".bytes) == ""
  end

  var chars = []
  for i in 0..data.size
    code = case data.get(i)
      Some(byte): byte.to_int
      None: stop "every position below the size holds a byte"
    end
    chars = chars.append(code.to_char or ' ')
  end
  Text.from_chars(chars)
end

## The words in `text`, in ASCII lowercase: runs of ASCII letters and
## apostrophes. Every other character separates words.
fn words_in(text: Text) : List(Text)
  examples
    words_in("It's the cat's hat, isn't it?") == ["it's", "the", "cat's", "hat", "isn't", "it"]
    words_in("Hello, HELLO hello!") == ["hello", "hello", "hello"]
    words_in("'Tis rock'n'roll") == ["'tis", "rock'n'roll"]
    words_in("naïve café-au-lait 42x") == ["na", "ve", "caf", "au", "lait", "x"]
    words_in("line one\nline\ttwo") == ["line", "one", "line", "two"]
    words_in("") == []
  end

  kept = text.chars.map(fn(c) if word_char?(c): c.ascii_lowercase else: ' ' end)
  Text.from_chars(kept).words
end

## Whether `c` can be part of a word: an ASCII letter or an apostrophe.
fn word_char?(c: Char) : Bool
  examples
    word_char?('a') and word_char?('Z') and word_char?('\'')
    not word_char?('-') and not word_char?('7') and not word_char?('é')
  end

  c.ascii_letter? or c == '\''
end

## The `top` most frequent words with their counts: the most frequent first,
## and equally frequent words in alphabetical order.
fn most_frequent(counts: Map(Text, Int), top: Int) : List((Text, Int))
  examples
    most_frequent({"b": 2, "a": 2, "c": 5, "d": 1}, 3) == [("c", 5), ("a", 2), ("b", 2)]
    most_frequent({"the": 1}, 10) == [("the", 1)]
    most_frequent({"the": 1}, 0) == []
  end

  # sorting on the negated count puts the most frequent first, and the word
  # breaks ties
  counts.entries
    .sort_by(fn(entry) (-entry.1, entry.0) end)
    .take(top)
end

test "words are counted across files, most frequent first, ties alphabetical"
  console = Console.fake()
  files = Files.in_memory({"a.txt": "The cat and the hat.", "b.txt": "THE CAT'S hat"}).read_only
  assert main(Args.of(["a.txt", "b.txt"]), console, files) == 0
  assert console.printed == ["3 the", "2 hat", "1 and", "1 cat", "1 cat's"]
  assert console.printed_errors == []
end

test "ten words are printed unless --top asks for another number"
  console = Console.fake()
  files = Files.in_memory({"a.txt": "l k j i h g f e d c b a b c"}).read_only
  assert main(Args.of(["a.txt"]), console, files) == 0
  assert console.printed == ["2 b", "2 c", "1 a", "1 d", "1 e", "1 f", "1 g", "1 h", "1 i", "1 j"]
  top_two = Console.fake()
  assert main(Args.of(["--top", "2", "a.txt"]), top_two, files) == 0
  assert top_two.printed == ["2 b", "2 c"]
end

test "a file that can't be read is reported, and the others are still counted"
  console = Console.fake()
  files = Files.in_memory({"a.txt": "one two two"}).read_only
  assert main(Args.of(["missing.txt", "a.txt"]), console, files) == 1
  assert console.printed == ["2 two", "1 one"]
  assert console.printed_errors == ["word-freq: missing.txt: no such file"]
end

test "a mistake on the command line is reported with the usage"
  console = Console.fake()
  files = Files.in_memory({"a.txt": "one"}).read_only
  assert main(Args.of(["--top", "lots", "a.txt"]), console, files) == 2
  assert console.printed == []
  assert console.printed_errors == ["word-freq: --top needs a whole number of 0 or more, not \"lots\"", usage]
end

test "with no files named, standard input is counted"
  console = Console.fake_input(["Hello hello", "world"])
  files = Files.in_memory({}).read_only
  assert main(Args.of([]), console, files) == 0
  assert console.printed == ["2 hello", "1 world"]
end
