## Counts newline bytes, ASCII-whitespace-delimited words and bytes in files.
module Wc

struct Counts
  lines: BigInt
  words: BigInt
  bytes: BigInt
end

fn count_bytes(data: Bytes) : Counts
  var counts = Counts(lines: 0, words: 0, bytes: data.size.to_big)
  var in_word? = false
  var position = 0
  while data.get(position) is Some(byte)
    code = byte.to_int
    if code == 10
      counts.lines += 1
    end
    white_space? = code == 32 or (code >= 9 and code <= 13)
    if white_space?
      in_word? = false
    else
      if not in_word?
        counts.words += 1
      end
      in_word? = true
    end
    position += 1
  end
  counts
end

fn main(args: Args, console: Console, files: ReadFiles) : Int
  var status = 0
  var total = Counts(lines: 0, words: 0, bytes: 0)
  for path in args.list
    case files.read_bytes(path)
      Ok(data):
        counts = count_bytes(data)
        console.print("#{counts.lines} #{counts.words} #{counts.bytes} #{path}")
        total.lines += counts.lines
        total.words += counts.words
        total.bytes += counts.bytes
      Error(problem):
        console.error("wc: #{problem}")
        status = 1
    end
  end
  if args.list.size > 1
    console.print("#{total.lines} #{total.words} #{total.bytes} total")
  end
  status
end
