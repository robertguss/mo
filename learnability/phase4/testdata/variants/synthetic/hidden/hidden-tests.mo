## The small queue's hidden tests: stats counts a dead job as dead.
module HiddenTests
use Main{Stats}
use FixtureSteps

t0 = Time.parse("2026-01-01T00:00:00Z") or stop "time"

test "stats counts the pre-state's dead job"
  sim = Simulation.start(Main, t0)
  FixtureSteps.replay(sim)
  assert Simulation.send(sim, Main.stats) ==
    Ok(Stats(total: 5, ready: 3, leased: 0, done: 1, dead: 1, limit: 5000, now: t0 + 1.second))
end
