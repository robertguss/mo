module Main
service
  state Int = stop "initializer must not run"
  tools snapshot
  token from env "MO_SERVICE_TEST_TOKEN_NOT_READ"
end
fn snapshot(state: Int, _now: Time) : List(Int)
  [state]
end
