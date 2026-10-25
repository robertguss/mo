module Main
service
  tools snapshot
  state Int = 0
  token from env "TOKEN"
end
fn snapshot(state: Int, _now: Time) : List(Int)
  [state]
end
