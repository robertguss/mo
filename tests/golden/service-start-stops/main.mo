module Main

service
  state List(Int) = stop "the start value stops before any restart"
  tools snapshots
  token from env "MO_SERVICE_TEST_TOKEN_NOT_READ"
end

fn snapshots(state: List(Int), _now: Time) : List(List(Int))
  [state]
end
