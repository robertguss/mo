## Counts ASCII words across files, ignoring ASCII case.
module WordFreq

fn count_words(data: Bytes, previous: Map(Text, Int)) : Map(Text, Int)
  var counts = previous
  var letters = []
  var position = 0
  while data.get(position) is Some(byte)
    code = byte.to_int
    if (code >= 65 and code <= 90) or
      (code >= 97 and code <= 122) or code == 39
      ch = case code.to_char
        Some(value): value.ascii_lowercase
        None: stop "ASCII letters and apostrophes are valid character codes"
      end
      letters = letters.append(ch)
    else if not letters.empty?
      word = Text.from_chars(letters)
      counts = counts.set(word, (counts.get(word) or 0) + 1)
      letters = []
    end
    position += 1
  end
  if not letters.empty?
    word = Text.from_chars(letters)
    counts = counts.set(word, (counts.get(word) or 0) + 1)
  end
  counts
end

fn main(args: Args, console: Console, files: ReadFiles) : Int
  var top = 10
  var paths = []
  var need_top? = false
  var options? = true
  for argument in args.list
    if need_top?
      case argument.to_int
        Some(n) if n >= 0: top = n
        Some(_) | None:
          console.error("word-freq: --top requires a nonnegative integer")
          return 1
      end
      need_top? = false
    else if options? and argument == "--top"
      need_top? = true
    else if options? and argument == "--"
      options? = false
    else if options? and argument.starts_with?("--")
      console.error("word-freq: unknown option #{argument}")
      return 1
    else
      paths = paths.append(argument)
    end
  end
  if need_top?
    console.error("word-freq: --top requires a nonnegative integer")
    return 1
  end
  if paths.empty?
    console.error("Usage: word-freq [--top N] [--] FILE...")
    return 1
  end

  var counts = {}
  var status = 0
  for path in paths
    case files.read_bytes(path)
      Ok(data): counts = count_words(data, counts)
      Error(problem):
        console.error("#{problem}")
        status = 1
    end
  end
  ranked = counts.entries.sort_by(fn(entry) (-entry.1, entry.0) end)
  for (word, count) in ranked.take(top)
    console.print("#{count} #{word}")
  end
  status
end
