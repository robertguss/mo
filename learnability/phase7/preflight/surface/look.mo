## Prints one API response readably: the first run of a spike.
##
## The token is read from the environment variable API_TOKEN.
module Look
use Http{HttpError}
use Network{Network}

fn spike(env: Env, console: Console, network: Network) : Result(Unit, HttpError)
  url = "https://api.example.com/repos/mo/mo/issues"
  token = env.get("API_TOKEN") or ""
  call = Http.get(url).query("state", "open").header("Authorization", "Bearer #{token}")
  response = try call.send(network)
  console.print("#{response.status}")
  issues = try response.json
  console.print(issues.pretty)
  Ok(Unit)
end
