module Main

service
  state Int = 0
  tools read
  token from env "TOKEN"
end

fn read(state: Int, _now: Time) : List(Int)
  [state]
end

fn make(at: Time) : Simulation
  Simulation.start(Main, at)
end
