module Main
fn run(error: NetworkError) : Int
case error
Network.UnknownHost(host: _): 0
Network.Refused(host: _, port: _): 1
Network.TimedOut: 2
end
end
