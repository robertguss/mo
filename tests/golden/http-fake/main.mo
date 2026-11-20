## Prints the titles of a repository's open issues.
module Main
use Http
use Json

fn titles(network: Network, repo: Text, token: Text) : Result(List(Text), Http.HttpError)
  url = "http://api.example.com/repos/#{repo}/issues"
  asked = Http.get(url).query("state", "open").header("Authorization", "Bearer #{token}")
  response = try asked.send(network)
  return Error(Http.BadResponse(reason: "status #{response.status}")) if not response.ok?
  issues = try response.json
  items = try issues.list_at("").or_fail(Http.BadResponse(reason: "not a list"))
  Ok(items.filter_map(fn(issue) issue.text_at("title") end))
end

fn main(args: Args, console: Console, network: Network) : Result(Unit, Http.HttpError)
  case args.list
    [repo]:
      for title in try titles(network, repo, "t0ken")
        console.print(title)
      end
    _: console.error("usage: issues OWNER/REPO")
  end
  Ok(Unit)
end

fn listing(token: Text) : Http.Request
  Http.get("http://api.example.com/repos/mo/mo/issues").header("Authorization", "Bearer #{token}")
    .query("state", "open")
end

test "a function that calls an API gets its answer from a fake network"
  page = "[{\"number\": 7, \"title\": \"Crash on empty input\"}, {\"number\": 9, \"title\": \"Slow start\"}]"
  network = Network.fake(Http.replies([(listing("t0ken"), Http.reply(200, page))]))
  assert titles(network, "mo/mo", "t0ken") == Ok(["Crash on empty input", "Slow start"])
end

test "a request the fake holds no reply for is an error that names it"
  network = Network.fake(Http.replies([(listing("t0ken"), Http.reply(200, "[]"))]))
  assert titles(network, "mo/mo", "t0ken") == Ok([])
  assert titles(network, "mo/other", "t0ken") is
    Error(Http.Unreachable(Network.Broken(reason: missing)))
  assert missing == "no reply for \"GET /repos/mo/other/issues?state=open HTTP/1.1\""
  assert titles(network, "mo/mo", "other") is
    Error(Http.Unreachable(Network.Broken(reason: differs)))
  assert differs ==
    "no reply for \"GET /repos/mo/mo/issues?state=open HTTP/1.1\": sent \"Authorization: Bearer other\\r\\n\", expected \"Authorization: Bearer t0ken\\r\\n\""
end

test "a status that is not a success is a response, and the function decides what it means"
  for status in [404, 500]
    network = Network.fake(Http.replies([(listing("t0ken"), Http.reply(status, "no"))]))
    assert listing("t0ken").send(network) is Ok(response)
    assert response.status == status
    assert not response.ok?
    assert response.text == Some("no")
    assert titles(network, "mo/mo", "t0ken") == Error(Http.BadResponse(reason: "status #{status}"))
  end
end

test "a body goes out as JSON or as text, and comes back the same way"
  saved = Http.post("http://example.org/items").json(Json.Object({"name": Json.String("pen")}))
  noted = Http.request("PUT", "http://example.org/notes/1").text("buy ink").within(2.seconds)
  answer = Http.reply(201, "{\"id\": 4}")
  network = Network.fake(Http.replies([(saved, answer), (noted, Http.reply(204, ""))]))
  assert saved.send(network) is Ok(created)
  assert created.status == 201
  assert created.header("content-length") == Some("9")
  assert created.json is Ok(body)
  assert body.int_at("id") == Some(4)
  assert noted.send(network) is Ok(done)
  assert done.status == 204
  assert done.text == Some("")
  assert noted.timeout == 2.seconds
end

test "a request the client can't send is an error before any connection"
  network = Network.fake({})
  assert Http.get("ftp://example.org/").send(network) ==
    Error(Http.BadUrl(url: "ftp://example.org/"))
  assert Http.get("http://example.org/").header("X-Token", "t0ken\n").send(network) ==
    Error(Http.BadHeader(name: "X-Token"))
  secure = Http.get("https://example.org/")
  assert secure.send(Network.fake(Http.replies([(secure, Http.reply(200, "ok"))]))) ==
    Ok(Http.reply(200, "ok"))
  assert secure.send(network) ==
    Error(Http.Unreachable(Network.UnknownHost(host: "example.org")))
  assert Http.get("http://example.org/").send(network) ==
    Error(Http.Unreachable(Network.UnknownHost(host: "example.org")))
end

test "a response past the client's limits is an error, and one at them is read"
  asked = Http.get("http://example.org/")
  huge = Http.Response(status: 200, headers: [("Content-Length", "16777217")], body: "".bytes)
  assert asked.send(Network.fake(Http.replies([(asked, huge)]))) ==
    Error(Http.TooLarge(limit: 16777216))
  padded = Http.Response(status: 200, headers: [("X-Pad", "x".repeat(65510))], body: "".bytes)
  assert asked.send(Network.fake(Http.replies([(asked, padded)]))) == Ok(padded)
  longer = Http.Response(status: 200, headers: [("X-Pad", "x".repeat(65511))], body: "".bytes)
  assert asked.send(Network.fake(Http.replies([(asked, longer)]))) ==
    Error(Http.BadResponse(reason: "the headers are longer than 65536 bytes"))
end
