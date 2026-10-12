## Prints file names, line numbers and lines containing a literal phrase.
module Find

fn main(args: Args, console: Console, files: ReadFiles) : Int
  var ignore_case? = false
  var count_only? = false
  var operands = args.list

  # Options precede the phrase; -- permits a phrase beginning with a dash.
  while operands.first is Some(option)
    if option == "--ignore-case" or option == "-i"
      ignore_case? = true
    else if option == "--count" or option == "-c"
      count_only? = true
    else if option == "--"
      operands = operands.drop(1)
      break
    else if option.starts_with?("-") and option != "-"
      console.error("find: unknown option #{option}")
      console.error("Usage: find [--ignore-case] [--count] PHRASE FILE...")
      return 2
    else
      break
    end
    operands = operands.drop(1)
  end

  var matched? = false
  var read_error? = false
  case operands
    [phrase, first_file, ..remaining_files]:
      needle = if ignore_case?: phrase.ascii_lowercase else: phrase
      for path in remaining_files.prepend(first_file)
        case files.read_text(path)
          Ok(text):
            var match_count = 0
            for (index, line) in text.lines.enumerate
              searchable = if ignore_case?: line.ascii_lowercase else: line
              if searchable.contains?(needle)
                matched? = true
                match_count += 1
                if not count_only?
                  console.print("#{path}:#{index + 1}:#{line}")
                end
              end
            end
            if count_only?
              console.print("#{path}:#{match_count}")
            end
          Error(problem):
            console.error("find: #{problem}")
            read_error? = true
        end
      end
    _:
      console.error("Usage: find [--ignore-case] [--count] PHRASE FILE...")
      return 2
  end
  if read_error?: 2 else if matched?: 0 else: 1
end
