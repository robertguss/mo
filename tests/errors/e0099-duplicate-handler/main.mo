module Main
service
  state Int = 0
  tools snapshot, snapshot
  token from env "TOKEN"
end
fn snapshot(state: Int, _now: Time) : List(Int)
  [state]
end
