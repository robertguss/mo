module Differ
use Json

fn main(args: Args, console: Console, files: ReadFiles) : Int
  case files.read_text(args.list.first or "corpus.txt")
    Error(problem):
      console.error("#{problem}")
      1
    Ok(corpus):
      var index = 0
      for document in corpus.split("\u{1D}")
        console.print("#{index} #{Json.parse(document)}")
        index += 1
      end
      0
  end
end
