module Main
use Http

fn main(console: Console) : Result(Unit, Http.HttpError)
  response = try Http.get("http://example.org/").send(network)
  console.print("#{response.status}")
  Ok(Unit)
end
