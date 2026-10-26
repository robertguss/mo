module Main
service
  state Int = "wrong"
  tools snapshot
  token from env "TOKEN"
end
fn snapshot(state: Int, _now: Time) : List(Int)
  [state]
end
