## Fetches one JSON document with the Http client in lib/http.mo and prints how many items
## and bytes it holds.
##
##   fetch PORT PATH
module Fetch
use Http

fn main(args: Args, console: Console, network: Network) : Int
  case args.list
    [port, path]: report(console, network, "http://127.0.0.1:#{port}#{path}")
    _:
      console.error("usage: fetch PORT PATH")
      2
  end
end

fn report(console: Console, network: Network, url: Text) : Int
  case counted(network, url)
    Ok(line):
      console.print(line)
      0
    Error(problem):
      console.error("fetch: #{problem}")
      1
  end
end

fn counted(network: Network, url: Text) : Result(Text, Http.HttpError)
  response = try Http.get(url).send(network)
  document = try response.json
  items = try document.list_at("").or_fail(Http.BadResponse(reason: "not an array"))
  Ok("#{items.size} items, #{response.body.size} bytes")
end
