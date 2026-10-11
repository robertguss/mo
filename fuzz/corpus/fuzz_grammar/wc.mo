## Counts the lines, words and bytes in each file named on the command line,
## like Unix wc, with a total when there is more than one file. A file that
## can't be read is reported, and the exit status is then 1.
module Wc

struct Counts
  lines: Int
  words: Int
  bytes: Int
end

fn count(text: Text) : Counts
  examples
    count("") == Counts(lines: 0, words: 0, bytes: 0)
    count("one two\nthree\n") == Counts(lines: 2, words: 3, bytes: 14)
    count("no newline") == Counts(lines: 0, words: 2, bytes: 10)
  end

  Counts(
    lines: text.chars.count(fn(c) c == '\n' end),
    words: text.words.size,
    bytes: text.byte_count
  )
end

fn add(a: Counts, b: Counts) : Counts
  Counts(lines: a.lines + b.lines, words: a.words + b.words, bytes: a.bytes + b.bytes)
end

fn row(counts: Counts, name: Text) : Text
  examples
    row(Counts(lines: 2, words: 3, bytes: 14), "a.txt") == "      2       3      14 a.txt"
  end

  "#{column(counts.lines)} #{column(counts.words)} #{column(counts.bytes)} #{name}"
end

fn column(n: Int) : Text
  "#{n}".pad_left(7)
end

fn main(args: Args, console: Console, files: ReadFiles) : Int
  var total = Counts(lines: 0, words: 0, bytes: 0)
  var failed? = false
  for name in args.list
    case files.read_text(name)
      Ok(text):
        counts = count(text)
        console.print(row(counts, name))
        total = add(total, counts)
      Error(failure):
        console.error("wc: #{failure}")
        failed? = true
    end
  end
  if args.list.size > 1
    console.print(row(total, "total"))
  end
  if failed?: 1 else: 0
end

test "a missing file is reported, and the others still counted"
  console = Console.fake()
  files = Files.in_memory({"a.txt": "one two\n", "b.txt": "three\n"}).read_only
  status = main(Args.of(["a.txt", "gone.txt", "b.txt"]), console, files)
  assert status == 1
  assert console.printed == [
    "      1       2       8 a.txt",
    "      1       1       6 b.txt",
    "      2       3      14 total"
  ]
  assert console.printed_errors == ["wc: gone.txt: no such file"]
end
