## wc: counts the lines, words and bytes in each file named on the command
## line, as Unix wc does.
##
## Each file gets a line on standard output: its line, word and byte counts,
## right-aligned in columns of one width, then its name. When more than one
## file is named, a last line gives their total, labelled `total`. A line is
## counted at each line feed, and a word is a run of bytes that aren't ASCII
## white space, so a file needn't be text to be counted.
##
## A file that can't be read is reported on standard error, as in
## `wc: notes.txt: no such file`; the other files are still counted, and the
## exit status is then 1. Files are named relative to the folder wc is started
## in, and a name that would leave it, such as `../notes.txt`, is not allowed.
## With no file named, wc prints how to use it and exits with status 2:
## standard input isn't read, because a console reads it as lines of text,
## which can't give an exact count of bytes.
module Wc

## The three numbers wc reports for a file, or for several files together.
struct Counts
  lines: Int
  words: Int
  bytes: Int
end

## Nothing counted yet: where a total starts.
no_counts = Counts(lines: 0, words: 0, bytes: 0)

## The byte that ends a line.
line_feed = 10

## Counts each file named in `args`, in order, printing a line for each file
## it reads and an error for each it can't, then the total when more than one
## is named. Returns the exit status: 0 when every file was counted, 1 when
## one couldn't be read, and 2 when none was named.
fn main(args: Args, console: Console, files: ReadFiles) : Int
  names = args.list
  if names.empty?
    console.error("usage: wc FILE...")
    return 2
  end

  # Every file is counted before anything is printed, so that the columns
  # can be made as wide as the widest number in them.
  var outcomes = []
  var total = no_counts
  var failed? = false
  for name in names
    outcome = files.read_bytes(name).map(tally)
    case outcome
      Ok(counts): total = total.plus(counts)
      Error(_): failed? = true
    end
    outcomes = outcomes.append((name, outcome))
  end

  # The total is at least as large as any one file's counts, so its widest
  # number sets the width of every column, whether it is printed or not.
  width = widest(total)
  for (name, outcome) in outcomes
    case outcome
      Ok(counts): console.print(row(counts, width, name))
      Error(problem): console.error("wc: #{problem}")
    end
  end
  if names.size > 1
    console.print(row(total, width, "total"))
  end
  if failed?: 1 else: 0
end

## Counts the lines, words and bytes in some data, as wc does: a line at each
## line feed, so a last line without one isn't counted, and a word for each
## run of bytes that aren't ASCII white space.
fn tally(data: Bytes) : Counts
  promise "every byte is counted, and there are never more lines or words than bytes"
    result.bytes == data.size
    result.lines <= result.bytes
    result.words <= result.bytes
    allows data: "one\n".bytes, result: Counts(lines: 1, words: 1, bytes: 4)
    forbids data: "one\n".bytes, result: Counts(lines: 1, words: 1, bytes: 3)
    forbids data: "one\n".bytes, result: Counts(lines: 1, words: 5, bytes: 4)
  end
  examples
    tally("".bytes) == no_counts
    tally("one two\nthree\n".bytes) == Counts(lines: 2, words: 3, bytes: 14)
    tally("  no line break at the end".bytes) == Counts(lines: 0, words: 6, bytes: 26)
    tally("a\tb\r\nc\u{000B}d\u{000C}e".bytes) == Counts(lines: 1, words: 5, bytes: 10)
    tally("\n\n\n".bytes) == Counts(lines: 3, words: 0, bytes: 3)
    tally("caf\u{E9}\n".bytes) == Counts(lines: 1, words: 1, bytes: 6)  # é is 2 bytes
  end

  var lines = 0
  var words = 0
  var in_word? = false
  var i = 0
  while data.get(i) is Some(byte)
    code = byte.to_int
    if code == line_feed
      lines += 1
    end
    if white_space?(code)
      in_word? = false
    else if not in_word?
      in_word? = true
      words += 1
    end
    i += 1
  end
  Counts(lines: lines, words: words, bytes: data.size)
end

## Whether a byte is ASCII white space: tab, line feed, vertical tab, form
## feed or carriage return (9 to 13), or space (32).
fn white_space?(code: Int) : Bool
  examples
    white_space?(' '.code)
    white_space?('\t'.code)
    white_space?('\n'.code)
    white_space?('\r'.code)
    not white_space?('x'.code)
    not white_space?(0)
    not white_space?(0xA0)
  end

  code == 32 or (code >= 9 and code <= 13)
end

## Two counts added together, field by field.
fn plus(a: Counts, b: Counts) : Counts
  examples
    plus(no_counts, no_counts) == no_counts
    plus(Counts(lines: 1, words: 2, bytes: 3), Counts(lines: 10, words: 20, bytes: 30)) ==
      Counts(lines: 11, words: 22, bytes: 33)
  end

  Counts(lines: a.lines + b.lines, words: a.words + b.words, bytes: a.bytes + b.bytes)
end

## How many characters the widest of some counts takes to write.
fn widest(counts: Counts) : Int
  examples
    widest(no_counts) == 1
    widest(Counts(lines: 12, words: 345, bytes: 6789)) == 4
  end

  counts.lines.max(counts.words).max(counts.bytes).to_text.char_count
end

## One line of the report: the counts, each right-aligned to `width`, then the
## label, with a space between each two.
fn row(counts: Counts, width: Int, label: Text) : Text
  examples
    row(Counts(lines: 2, words: 3, bytes: 14), 2, "a.txt") == " 2  3 14 a.txt"
    row(Counts(lines: 2, words: 3, bytes: 14), 4, "total") == "   2    3   14 total"
    row(Counts(lines: 2, words: 3, bytes: 14), 1, "a.txt") == "2 3 14 a.txt"
  end

  numbers = [counts.lines, counts.words, counts.bytes].map(fn(n) n.to_text.pad_left(width) end)
  "#{numbers.join(" ")} #{label}"
end

test "one file gets one line, and no total"
  console = Console.fake()
  files = Files.in_memory({"poem.txt": "one two\nthree\n"}).read_only
  assert main(Args.of(["poem.txt"]), console, files) == 0
  assert console.printed == [" 2  3 14 poem.txt"]
  assert console.printed_errors == []
end

test "several files get a total, and every column is as wide as its widest number"
  console = Console.fake()
  files = Files.in_memory({
    "poem.txt": "one two\nthree\n",
    "long.txt": "word ".repeat(30)
  }).read_only
  assert main(Args.of(["poem.txt", "long.txt"]), console, files) == 0
  assert console.printed == [
    "  2   3  14 poem.txt",
    "  0  30 150 long.txt",
    "  2  33 164 total"
  ]
  assert console.printed_errors == []
end

test "a file that can't be read is reported, the others are counted, and the status is 1"
  console = Console.fake()
  files = Files.in_memory({"a.txt": "one two\nthree\n", "b.txt": "four"}).read_only
  assert main(Args.of(["a.txt", "missing.txt", "b.txt", "../c.txt"]), console, files) == 1
  assert console.printed == [" 2  3 14 a.txt", " 0  1  4 b.txt", " 2  4 18 total"]
  assert console.printed_errors == [
    "wc: missing.txt: no such file",
    "wc: ../c.txt: not allowed"
  ]
end

test "the total is printed even when no file could be read"
  console = Console.fake()
  files = Files.in_memory({}).read_only
  assert main(Args.of(["x.txt", "y.txt"]), console, files) == 1
  assert console.printed == ["0 0 0 total"]
  assert console.printed_errors == ["wc: x.txt: no such file", "wc: y.txt: no such file"]
end

test "with no file named, wc says how to use it, and the status is 2"
  console = Console.fake()
  files = Files.in_memory({}).read_only
  assert main(Args.of([]), console, files) == 2
  assert console.printed == []
  assert console.printed_errors == ["usage: wc FILE..."]
end
