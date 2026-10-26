module Main

initial = 7

service
  state Int = identity(initial)
  tools snapshot
  every 2.minutes tick
  token from env "SERVICE_TOKEN"
end

fn identity(value: T) : T
  value
end

fn snapshot(state: Int, _now: Time) : List(Int)
  [state]
end

fn tick(state: Int, _now: Time) : Int
  state + 1
end

fn main() : Int
  19
end
