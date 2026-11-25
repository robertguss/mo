## Fault-injecting scripted fake networks and recorded-call inspection for tests.
module Network
expose FakeReply(..), RecordedCall(..), fake_scripted, recorded_calls
use Http

enum FakeReply
  Respond(response: Http.Response)
  Fail(error: NetworkError)
  TruncateAfter(bytes: Int)
end

struct RecordedCall
  method: Text
  url: Text
  headers: List((Text, Text))
  body: Bytes
end

fn format_response(response: Http.Response) : Bytes
  lines = response.headers.map(fn(h) "#{h.0}: #{h.1}\r\n" end)
  Bytes.join(["HTTP/1.1 #{response.status} \r\n#{lines.join("")}\r\n".bytes, response.body])
end

fn encode_reply(reply: FakeReply) : (Int, Int, Bytes, NetworkError)
  case reply
    Respond(response: res): (0, 0, format_response(res), Network.TimedOut)
    Fail(error: err): (1, 0, Bytes.empty, err)
    TruncateAfter(bytes: count): (2, count, Bytes.empty, Network.TimedOut)
  end
end

fn encode_scripts(scripts: List((Text, Text, List(FakeReply)))) : List(
  (Text, Text, List((Int, Int, Bytes, NetworkError)))
)
  scripts.map(fn(s) (s.0, s.1, s.2.map(fn(r) encode_reply(r) end)) end)
end

fn decode_recorded_calls(raw: List((Text, Text, List((Text, Text)), Bytes))) : List(RecordedCall)
  raw.map(fn(r) RecordedCall(method: r.0, url: r.1, headers: r.2, body: r.3) end)
end

## Builds a fake `Network` with ordered response or fault queues matched by `(method, url_prefix)`.
fn fake_scripted(scripts: List((Text, Text, List(FakeReply)))) : Network
  if scripts.empty?
    stop "compiler-lowered Network.fake_scripted"
  else
    stop "compiler-lowered Network.fake_scripted"
  end
end

## Returns every HTTP call sent over a fake `Network` in the order it was sent.
fn recorded_calls(net: Network) : List(RecordedCall)
  _ = net
  stop "compiler-lowered Network.recorded_calls"
end

test "fake_scripted plays ordered replies, faults, truncations, and records calls"
  net = Network.fake_scripted([
    (
      "POST",
      "https://api.example.org/v1/charges",
      [
        Fail(error: Network.TimedOut),
        Respond(response: Http.reply(502, "bad gateway")),
        Respond(response: Http.reply(200, "{\"ok\":true}"))
      ]
    ),
    ("GET", "https://api.example.org/v1/cut", [TruncateAfter(bytes: 4)])
  ])

  req = Http.post("https://api.example.org/v1/charges").header("Authorization", "Bearer t0ken").text(
    "{\"amount\":100}"
  )

  assert req.send(net) == Error(Http.Unreachable(Network.TimedOut))
  assert req.send(net) == Ok(Http.reply(502, "bad gateway"))
  assert req.send(net) == Ok(Http.reply(200, "{\"ok\":true}"))
  assert req.send(net) == Ok(Http.reply(200, "{\"ok\":true}"))

  cut_res = Http.get("https://api.example.org/v1/cut").send(net)
  assert cut_res ==
    Error(Http.BadResponse(reason: "the connection closed before the response was whole"))

  unmatched = Http.get("https://other.example.org/v1/none").send(net)
  assert unmatched == Error(Http.Unreachable(Network.Refused(host: "other.example.org", port: 443)))

  calls = Network.recorded_calls(net)
  assert calls.size == 6
  first = calls.get(0) or stop "missing first call"
  assert first.method == "POST"
  assert first.url == "https://api.example.org/v1/charges"
  assert first.body == "{\"amount\":100}".bytes
  assert first.headers.any?(fn(h) h == ("Authorization", "Bearer t0ken") end)
end

test "allow_hosts narrows fake_scripted fail-closed while sharing script state and recorded calls"
  base = Network.fake_scripted([
    ("GET", "https://allowed.example.org/ok", [Respond(response: Http.reply(200, "allowed"))]),
    ("GET", "https://blocked.example.org/secret", [Respond(response: Http.reply(200, "leaked"))])
  ])
  narrowed = base.allow_hosts(["allowed.example.org"])

  assert Http.get("https://blocked.example.org/secret").send(narrowed) ==
    Error(Http.Unreachable(Network.Refused(host: "blocked.example.org", port: 443)))
  assert Http.get("https://allowed.example.org/ok").send(narrowed) == Ok(Http.reply(200, "allowed"))

  calls = narrowed.recorded_calls()
  assert calls.size == 1
  only = calls.get(0) or stop "missing allowed call"
  assert only.url == "https://allowed.example.org/ok"
  assert Network.recorded_calls(base) == calls
end
