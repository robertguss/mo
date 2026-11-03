module Main

service
  state Int = 0
  tools add
  token from env "TOKEN"
end

fn add(state: Int, _now: Time, amount: Int) : (Int, Int)
  (state + amount, state)
end

fn echo(_sim: Simulation, n: Int) : Int
  promise "it gives n back"
    result == n
    allows _sim: Simulation.start(Main, t0), n: 1, result: 1
    forbids _sim: Simulation.start(Main, t0), n: 1, result: 2
  end

  n
end

t0 = Time.parse("2026-01-01T00:00:00Z") or stop "time"
