module Main
service
  state Int = 0
  tools increment
  token from env "TOKEN"
end
## Add one.
fn increment(state: Int, _now: Time) : Int
  state + 1
end
fn apply(state: Int, now: Time) : Int
  alias = increment
  alias(state, now)
end
