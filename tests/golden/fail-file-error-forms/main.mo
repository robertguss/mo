module Main

fn show(result: Result(Text, FileError)) : Text
  case result
    Ok(text): "ok #{text}"
    Error(problem): "#{problem}"
  end
end

fn main(console: Console)
  errors = [
    NotFound(path: "notes/a.md"),
    NotAllowed(path: "../a.txt"),
    NotText(path: "photo.jpg"),
    Failed(path: "logs", reason: "is a folder")
  ]
  for problem in errors
    console.print(problem.to_text)
  end
  console.print(show(Error(NotAllowed(path: "../x"))))
  console.print("#{[NotText(path: "p")]}")
end
