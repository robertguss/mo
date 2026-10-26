module Counter

service
  state Int = 0
  tools count, add
  every 1.minute tick
  token from env "COUNTER_TOKEN"
end

fn tick(state: Int, now: Time) : Int
  state
end
