## A stand-in for the proposed Network and Connection capability types, so that the proposed
## forms can be type-checked today. Nothing here runs.
module Network
expose Network, Connection, NetworkError(..), connect, send, receive, fake

struct Network
  peers: Map(Text, Map(Bytes, Bytes))
end

struct Connection
  address: Text
end

enum NetworkError
  UnknownHost(host: Text)
  Refused(host: Text, port: Int)
  TimedOut
  Closed
  Broken(reason: Text)
end

fn connect(_network: Network, _host: Text, _port: Int, _within: Duration) : Result(
  Connection,
  NetworkError
)
  stop "a stand-in"
end

fn send(_connection: Connection, _bytes: Bytes) : Result(Unit, NetworkError)
  stop "a stand-in"
end

fn receive(_connection: Connection, _most: Int) : Result(Bytes, NetworkError)
  stop "a stand-in"
end

fn fake(peers: Map(Text, Map(Bytes, Bytes))) : Network
  Network(peers: peers)
end
