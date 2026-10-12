## Finds literal phrases in text files, optionally folding ASCII case.
module Find

struct Options
  ignore_case?: Bool
  count_only?: Bool
  phrase: Text
  paths: List(Text)
end

fn options(arguments: List(Text)) : Option(Options)
  var ignore_case? = false
  var count_only? = false
  var position = 0
  for argument in arguments
    if argument == "--ignore-case" or argument == "-i"
      ignore_case? = true
    else if argument == "--count" or argument == "-c"
      count_only? = true
    else if argument == "--"
      position += 1
      break
    else
      return None if argument.starts_with?("-")
      break
    end
    position += 1
  end
  phrase = try arguments.get(position)
  paths = arguments.drop(position + 1)
  return None if paths.empty?
  Some(Options(ignore_case?: ignore_case?, count_only?: count_only?,
    phrase: phrase, paths: paths))
end

fn main(args: Args, console: Console, files: ReadFiles) : Int
  settings = case options(args.list)
    Some(value): value
    None:
      console.error("Usage: find [--ignore-case|-i] [--count|-c] [--] PHRASE FILE...")
      return 2
  end
  needle = if settings.ignore_case?: settings.phrase.ascii_lowercase else: settings.phrase
  var matched? = false
  var failed? = false
  for path in settings.paths
    case files.read_text(path)
      Ok(text):
        var count = 0
        for (index, line) in text.lines.enumerate
          haystack = if settings.ignore_case?: line.ascii_lowercase else: line
          if haystack.contains?(needle)
            count += 1
            matched? = true
            if not settings.count_only?
              console.print("#{path}:#{index + 1}:#{line}")
            end
          end
        end
        if settings.count_only?
          console.print("#{path}:#{count}")
        end
      Error(problem):
        console.error("#{problem}")
        failed? = true
    end
  end
  if failed?: 2 else if matched?: 0 else: 1
end
