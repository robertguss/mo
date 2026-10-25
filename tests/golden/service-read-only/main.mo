module Main

service
  state List(Int) = stop "service initialization must not run during tests"
  tools snapshots
  every 1.minute tick
  token from env "MO_SERVICE_TEST_TOKEN_NOT_READ"
end

fn snapshots(state: List(Int), _now: Time) : List(List(Int))
  [state]
end

fn tick(state: List(Int), _now: Time) : List(Int)
  state
end

test "handlers remain ordinary functions"
  now = Time.parse("2026-10-25T00:00:00Z") or stop "valid time"
  assert snapshots([3, 7], now) == [[3, 7]]
  assert tick([2, 9], now) == [2, 9]
end
