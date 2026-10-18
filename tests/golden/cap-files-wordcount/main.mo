module Main

fn count_words(files: ReadFiles, path: Text) : Result(Int, FileError)
  text = try files.read_text(path)
  Ok(text.words.size)
end

fn main(args: Args, console: Console, files: Files) : Int
  var failures = 0
  for path in args.list
    case count_words(files.read_only, path)
      Ok(n): console.print("#{path}: #{n}")
      Error(failure):
        console.error("count: #{failure}")
        failures += 1
    end
  end
  failures
end
