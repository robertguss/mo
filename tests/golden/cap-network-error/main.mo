module Main

enum Door
  Refused(why: Text)
  Open
end

fn name(error: NetworkError) : Text
  case error
    Network.UnknownHost(host: host): "unknown #{host}"
    Network.Refused(host: host, port: port): "refused #{host} #{port}"
    Network.TimedOut: "timed out"
    Network.Closed: "closed"
    Network.Broken(reason: reason): "broken #{reason}"
    Network.Certificate(reason: reason): "certificate #{reason}"
  end
end

fn main(console: Console, _network: Network)
  refused = Network.Refused(host: "h", port: 80)
  all = [
    Network.UnknownHost(host: "nowhere"),
    refused,
    Network.TimedOut,
    Network.Closed,
    Network.Broken(reason: "bad line"),
    Network.Certificate(reason: "expired")
  ]
  console.print(all.map(name).join(", "))
  console.print("#{all}")
  console.print("#{refused == Network.Refused(host: "h", port: 80)} #{refused == Network.TimedOut}")
  console.print(
    "#{refused < Network.Refused(host: "h", port: 443)} #{Network.Closed.compare(Network.TimedOut)}"
  )
  door = Refused(why: "locked")
  console.print("#{door} #{door is Refused(why: _)} #{Open}")
  console.print("#{refused is Network.Refused(host: _, port: 80)} #{refused is Network.Closed}")
end
