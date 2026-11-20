module Main
use Http

fn fetch_allowed(network: Network, url: Text) : Result(Int, Http.HttpError)
  narrowed = network.allow_hosts(["api.example.org:80", "cdn.example.org"])
  response = try Http.get(url).send(narrowed)
  Ok(response.status)
end

fn main(console: Console) : Int
  console.print("allow-hosts")
  0
end

test "allow_hosts narrows fake network monotonically"
  peers = Http.replies([
    (Http.get("http://api.example.org/v1"), Http.reply(200, "ok")),
    (Http.get("http://evil.example.org/v1"), Http.reply(200, "leaked"))
  ])
  net = Network.fake(peers)
  assert fetch_allowed(net, "http://api.example.org/v1") == Ok(200)
  assert fetch_allowed(net, "http://evil.example.org/v1") ==
    Error(Http.Unreachable(Network.Refused(host: "evil.example.org", port: 80)))
  further = net.allow_hosts(["api.example.org:80"]).allow_hosts(["evil.example.org"])
  assert Http.get("http://api.example.org/v1").send(further) ==
    Error(Http.Unreachable(Network.Refused(host: "api.example.org", port: 80)))
end
