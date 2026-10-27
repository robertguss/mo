module Main

service
  state Int = 0
  tools add
  token from env "TOKEN"
end

fn add(state: Int, _now: Time, amount: Int) : (Int, Int)
  (state + amount, state)
end

test "simulation lifecycle"
  sim = Simulation.start(Main, Time.parse("2026-01-01T00:00:00Z") or stop "time")
  assert Simulation.send(sim, Main.add, 3) == Ok((3, 0))
  Simulation.advance(sim, 1.minute)
  Simulation.crash(sim)
  Simulation.restart(sim)
  assert Simulation.send(sim, Main.add, 5) == Ok((8, 3))
end

fn main() : Int
  0
end
