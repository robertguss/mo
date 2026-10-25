module Main
service
  state Int = 0
  tools snapshot
  token from env "TOKEN"
end
fn snapshot(state: Int) : List(Int)
  [state]
end
