module Main

test "selector"
  _sim = Simulation.start(Main, Time.parse("2026-01-01T00:00:00Z") or stop "time")
end
