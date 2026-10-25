module Main
service
  state Int = 0
  tools snapshot
  every 0.seconds tick
  token from env "TOKEN"
end
fn snapshot(state: Int, _now: Time) : List(Int)
  [state]
end
fn tick(state: Int, _now: Time) : Int
  state
end
