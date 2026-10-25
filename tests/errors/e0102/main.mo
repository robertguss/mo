module Main
service
  state Int = 0
  tools snapshot
  token from env "1TOKEN"
end
fn snapshot(state: Int, _now: Time) : List(Int)
  [state]
end
