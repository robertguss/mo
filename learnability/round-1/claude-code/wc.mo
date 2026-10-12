## Counts the lines, words and bytes in each file named on the command line,
## as Unix wc does, and prints their total when more than one file is named.
## A file that can't be read is reported on standard error, the other files
## are still counted, and the exit status is then 1. With no file named, it
## counts standard input instead.
##
## Each line of output gives the lines, words and bytes, right-aligned in
## columns wide enough for the largest count, and then the file's name:
##
##    2  3 14 a.txt
##    1  1  5 b.txt
##    3  4 19 total
module Wc

## How many lines, words and bytes there are in some data. As in Unix wc, a
## line is counted for each line feed, so a last line without one isn't.
struct Counts
  lines: Int
  words: Int
  bytes: Int
end

## Nothing counted yet: where a total starts.
nothing_counted = Counts(lines: 0, words: 0, bytes: 0)

## The byte that ends a line.
line_feed = '\n'.code

## Prints the counts of each file named on the command line, then their total
## when more than one is named. Files that can't be read are reported on
## standard error. The exit status is 1 if a file couldn't be read, else 0.
fn main(args: Args, console: Console, files: ReadFiles) : Int
  names = args.list
  if names.empty?
    input = tally_input(console)
    console.print(columns(input, width_for(input)))
    return 0
  end

  # Every file is counted before any is printed, so that the columns can be
  # made wide enough for the total.
  var results = []
  var total = nothing_counted
  for name in names
    counted = files.read_bytes(name).map(tally)
    if counted is Ok(counts)
      total = add(total, counts)
    end
    results = results.append((name, counted))
  end

  width = width_for(total)
  var status = 0
  for (name, counted) in results
    case counted
      Ok(counts): console.print("#{columns(counts, width)} #{name}")
      Error(problem):
        status = 1
        console.error("wc: #{problem}")
    end
  end
  if names.size > 1
    console.print("#{columns(total, width)} total")
  end
  status
end

## Counts the lines, words and bytes in some data, as Unix wc does: each line
## feed ends a line, and a word is a run of bytes that aren't white space.
fn tally(data: Bytes) : Counts
  promise "every byte is counted, and there are never more lines or words than bytes"
    result.bytes == data.size
    result.lines <= result.bytes
    result.words <= result.bytes
    allows data: "a b\n".bytes, result: Counts(lines: 1, words: 2, bytes: 4)
    forbids data: "a b\n".bytes, result: Counts(lines: 1, words: 2, bytes: 5)
    forbids data: "\n".bytes, result: Counts(lines: 2, words: 0, bytes: 1)
    forbids data: "a".bytes, result: Counts(lines: 0, words: 2, bytes: 1)
  end
  examples
    tally("".bytes) == nothing_counted
    tally("one two\nthree\n".bytes) == Counts(lines: 2, words: 3, bytes: 14)
    tally("  no break at the end".bytes) == Counts(lines: 0, words: 5, bytes: 21)
    tally("tab\there\r\n".bytes) == Counts(lines: 1, words: 2, bytes: 10)
  end

  var lines = 0
  var words = 0
  var in_word? = false
  for i in 0..data.size
    code = case data.get(i)
      Some(byte): byte.to_int
      None: stop "every position below the size holds a byte"
    end
    if code == line_feed
      lines += 1
    end
    if white_space?(code)
      in_word? = false
    else if not in_word?
      words += 1
      in_word? = true
    end
  end
  Counts(lines: lines, words: words, bytes: data.size)
end

## Counts standard input. It can only be read a line at a time, without line
## breaks, so each line is counted as ending in a single line feed.
fn tally_input(console: Console) : Counts
  examples
    tally_input(Console.fake_input(["a b", "c"])) == Counts(lines: 2, words: 3, bytes: 6)
  end

  var counts = nothing_counted
  while console.read_line is Some(line)
    counts = add(counts, tally("#{line}\n".bytes))
  end
  counts
end

## Whether a byte is ASCII white space, which separates words: tab, line feed,
## vertical tab, form feed or carriage return (9 to 13), or space (32).
fn white_space?(code: Int) : Bool
  examples
    white_space?(' '.code)
    white_space?('\t'.code)
    white_space?('\r'.code)
    not white_space?(8)
    not white_space?('a'.code)
  end

  code == 32 or (code >= 9 and code <= 13)
end

## Two counts added together, as for a total.
fn add(a: Counts, b: Counts) : Counts
  Counts(
    lines: a.lines + b.lines,
    words: a.words + b.words,
    bytes: a.bytes + b.bytes
  )
end

## Counts as wc prints them: lines, words and bytes, each right-aligned in a
## column of the given width, with a space between the columns.
fn columns(counts: Counts, width: Int) : Text
  examples
    columns(Counts(lines: 2, words: 3, bytes: 14), 2) == " 2  3 14"
    columns(Counts(lines: 12, words: 345, bytes: 6789), 4) == "  12  345 6789"
  end

  [counts.lines, counts.words, counts.bytes]
    .map(fn(n) n.to_text.pad_left(width) end)
    .join(" ")
end

## How wide the columns must be for some counts: as wide as the byte count,
## since there are never more lines or words than bytes.
fn width_for(counts: Counts) : Int
  examples
    width_for(nothing_counted) == 1
    width_for(Counts(lines: 2, words: 3, bytes: 14)) == 2
  end

  counts.bytes.to_text.byte_count
end

test "each file is counted, and then the total"
  console = Console.fake()
  files = Files.in_memory({
    "a.txt": "one two\nthree\n",
    "b.txt": "four\n"
  }).read_only
  assert main(Args.of(["a.txt", "b.txt"]), console, files) == 0
  assert console.printed == [
    " 2  3 14 a.txt",
    " 1  1  5 b.txt",
    " 3  4 19 total"
  ]
  assert console.printed_errors == []
end

test "one file has no total, and a last line with no line feed is not a line"
  console = Console.fake()
  files = Files.in_memory({"a.txt": "one two\nthree"}).read_only
  assert main(Args.of(["a.txt"]), console, files) == 0
  assert console.printed == [" 1  3 13 a.txt"]
end

test "a file that cannot be read is reported, the others are counted, and the status is 1"
  console = Console.fake()
  files = Files.in_memory({"a.txt": "one two\nthree\n"}).read_only
  assert main(Args.of(["missing.txt", "a.txt"]), console, files) == 1
  assert console.printed == [" 2  3 14 a.txt", " 2  3 14 total"]
  assert console.printed_errors == ["wc: #{NotFound(path: "missing.txt")}"]
end

test "with no file named, standard input is counted"
  console = Console.fake_input(["one two", "three"])
  files = Files.in_memory({}).read_only
  assert main(Args.of([]), console, files) == 0
  assert console.printed == [" 2  3 14"]
end
