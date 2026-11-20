module Main

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

fn main(console: Console) : Int
  refused = Network.Refused(host: "h", port: 80)
  all = [
    Network.UnknownHost(host: "nowhere"),
    refused,
    Network.TimedOut,
    Network.Closed,
    Network.Broken(reason: "bad line"),
    Network.Certificate(reason: "expired")
  ]
  console.print("#{all}")
  if name(Network.UnknownHost(host: "nowhere")) == "unknown nowhere" and
    name(refused) == "refused h 80" and
    name(Network.TimedOut) == "timed out" and
    name(Network.Closed) == "closed" and
    name(Network.Broken(reason: "bad line")) == "broken bad line" and
    name(Network.Certificate(reason: "expired")) == "certificate expired" and
    "#{all}" ==
      "[UnknownHost(host: \"nowhere\"), Refused(host: \"h\", port: 80), TimedOut, Closed, Broken(reason: \"bad line\"), Certificate(reason: \"expired\")]" and
    refused == Network.Refused(host: "h", port: 80) and
    refused != Network.TimedOut and
    refused < Network.Refused(host: "h", port: 443) and
    Network.Closed.compare(Network.TimedOut) == Greater and
    refused is Network.Refused(host: _, port: 80): 0 else: 1
end

test "structural text of a network error"
  assert Network.Refused(host: "h", port: 80) == Network.Broken(reason: "r")
end
