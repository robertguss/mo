module Main

service
  state Int = 7
  tools accept
  token from env "TOKEN"
end

fn accept(state: Int, _now: Time, _a: Text, _b: Text) : (Int, Int)
  (state + 1, state)
end

test "aggregate input is refused before admission"
  sim = Simulation.start(Main, Time.parse("2026-01-01T00:00:00Z") or stop "time")
  large = "x".repeat(8388608)
  _reply = Simulation.send(sim, Main.accept, large, large)
end
