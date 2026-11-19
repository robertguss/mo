module Main

fn fetch(network: Network, within: Duration, request: Bytes) : Result(Bytes, NetworkError)
  connection = try network.connect("example.org", 80, within)
  try connection.send(request)
  connection.receive(4)
end

fn reaches?(network: Network, host: Text, within: Duration) : Bool
  promise "a generated network holds no peer, so it reaches no host"
    not result
    allows network: Network.fake({}), host: "example.org", within: 1.second, result: false
    forbids network: Network.fake({}), host: "example.org", within: 1.second, result: true
  end

  network.connect(host, 80, within) is Ok(_)
end

fn reason(result: Result(Bytes, NetworkError)) : Text
  if result is Error(Network.Broken(reason: why))
    why
  else
    "not broken"
  end
end

test "a fake answers a request it holds, at most four bytes at a time"
  asked = "GET /a HTTP/1.1\r\nAccept: a\r\n\r\n".bytes
  network = Network.fake({"example.org:80": {asked: "0123456789".bytes}})
  assert fetch(network, 0.seconds, asked) == Ok("0123".bytes)
  assert network.connect("example.org", 80, 0.seconds) is Ok(connection)
  assert connection.send("GET /a HTTP/1.1\r\n".bytes) == Ok(Unit)
  assert connection.send("Accept: a\r\n\r\n".bytes) == Ok(Unit)
  assert connection.receive(4) == Ok("0123".bytes)
  assert connection.receive(4) == Ok("4567".bytes)
  assert connection.receive(4) == Ok("89".bytes)
  assert connection.receive(4) == Ok("".bytes)
  assert connection.send(asked) == Error(Network.Closed)
end

test "a request the fake holds no reply for is an error that says what differs"
  asked = "GET /a HTTP/1.1\r\nAccept: a\r\n\r\n".bytes
  network = Network.fake({"example.org:80": {asked: "0123456789".bytes}})
  other = "no reply for \"GET /b HTTP/1.1\""
  differs = "no reply for \"GET /a HTTP/1.1\": sent \"Accept: b\\r\\n\", expected \"Accept: a\\r\\n\""
  assert reason(fetch(network, 0.seconds, "GET /b HTTP/1.1\r\n\r\n".bytes)) == other
  assert reason(fetch(network, 0.seconds, "GET /a HTTP/1.1\r\nAccept: b\r\n\r\n".bytes)) == differs
  assert network.connect("example.org", 80, 0.seconds) is Ok(silent)
  assert reason(silent.receive(4)) == "no reply for a connection that sent nothing"
  assert silent.send(asked) == Error(Network.Closed)
end

test "a host or a port the fake lacks is unknown, and two fakes share nothing"
  asked = "GET / HTTP/1.1\r\n\r\n".bytes
  one = Network.fake({"example.org:80": {asked: "one".bytes}})
  other = Network.fake({"example.org:80": {asked: "other".bytes}})
  assert one.connect("example.com", 80, 1.second) is Error(Network.UnknownHost(host: missing))
  assert missing == "example.com"
  assert one.connect("example.org", 81, 1.second) is Error(Network.UnknownHost(host: _))
  assert fetch(one, 1.second, asked) == Ok("one".bytes)
  assert fetch(other, 1.second, asked) == Ok("othe".bytes)
  assert reaches?(one, "example.org", 1.second)
  assert not reaches?(one, "example.com", 1.second)
end
