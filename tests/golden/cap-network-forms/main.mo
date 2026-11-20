module Main

test "every form of the new functions runs"
  bytes = "abcde".bytes
  cut = Bytes.slice
  join = Bytes.join
  assert Bytes.slice(bytes, 1, 3) == "bc".bytes
  assert cut(bytes, 1, 3) == "bc".bytes
  assert join([bytes, bytes]).size == 10
  asked = "a\n".bytes
  network = Network.fake({"h:1": {asked: "reply".bytes}, "h:443": {asked: "secure".bytes}})
  connect = Network.connect
  connect_secure = Network.connect_secure
  send = Connection.send
  receive = Connection.receive
  assert Network.connect(network, "h", 1, 1.second) is Ok(first)
  assert Connection.send(first, asked) == Ok(Unit)
  assert Connection.receive(first, 9) == Ok("reply".bytes)
  assert connect(network, "h", 1, 1.second) is Ok(second)
  assert send(second, asked) == Ok(Unit)
  assert receive(second, 9) == Ok("reply".bytes)
  assert Network.connect_secure(network, "h", 443, 1.second) is Ok(third)
  assert send(third, asked) == Ok(Unit)
  assert receive(third, 9) == Ok("secure".bytes)
  assert connect_secure(network, "h", 443, 1.second) is Ok(fourth)
  assert send(fourth, asked) == Ok(Unit)
  assert receive(fourth, 9) == Ok("secure".bytes)
  error = Network.Refused(host: "h", port: 1)
  assert "#{error}" == "Refused(host: \"h\", port: 1)"
  assert error.to_text == "Refused(host: \"h\", port: 1)"
  assert NetworkError.to_text(error) == "Refused(host: \"h\", port: 1)"
  assert error.compare(Network.TimedOut) == Less
  assert NetworkError.compare(error, Network.TimedOut) == Less
end
