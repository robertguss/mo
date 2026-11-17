## Reads a JSON file and parses it, to show what Json.parse costs with no HTTP.
##
##   parse FILE
module Parse
use Json

fn main(args: Args, console: Console, files: ReadFiles) : Int
  case files.read_text(args.list.first or "body.json")
    Error(problem):
      console.error("#{problem}")
      1
    Ok(body):
      case Json.parse(body)
        Ok(Json.Array(items)):
          console.print("#{items.size} items, #{body.byte_count} bytes")
          0
        Ok(_) | Error(_):
          console.error("json: not an array")
          1
      end
  end
end
