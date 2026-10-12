## Counts LF line endings, ASCII-whitespace-delimited words, and raw bytes.
module Wc

struct Counts
  lines: Int
  words: Int
  bytes: Int
end

fn count_bytes(data: Bytes) : Counts
  var line_count = 0
  var word_count = 0
  var inside_word? = false
  for index in 0..data.size
    byte = case data.get(index)
      Some(value): value.to_int
      None: stop "the index is inside the byte buffer"
    end
    if byte == 10
      line_count += 1
    end
    white? = byte == 32 or (byte >= 9 and byte <= 13)
    if not white? and not inside_word?
      word_count += 1
    end
    inside_word? = not white?
  end
  Counts(lines: line_count, words: word_count, bytes: data.size)
end

fn print_counts(counts: Counts, name: Text, console: Console)
  console.print("#{counts.lines} #{counts.words} #{counts.bytes} #{name}")
end

fn main(args: Args, console: Console, files: ReadFiles) : Int
  if args.list.empty?
    console.error("Usage: wc FILE...")
    return 1
  end
  var totals = Counts(lines: 0, words: 0, bytes: 0)
  var status = 0
  for name in args.list
    case files.read_bytes(name)
      Ok(data):
        counts = count_bytes(data)
        print_counts(counts, name, console)
        totals.lines += counts.lines
        totals.words += counts.words
        totals.bytes += counts.bytes
      Error(problem):
        console.error("#{problem}")
        status = 1
    end
  end
  if args.list.size > 1
    print_counts(totals, "total", console)
  end
  status
end
