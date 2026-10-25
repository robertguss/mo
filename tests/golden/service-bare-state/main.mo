module Main
expose increment
service
  state Int = 0
  tools increment
  token from env "TOKEN"
end
fn increment(state: Int, _now: Time) : Int
  state + 1
end
