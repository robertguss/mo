module Main
use Ticks
use Controls
use Client

service
  state Int = 0
  tools add, snapshot, reject, defect, busy
  token from env "TOKEN"
end

fn add(state: Int, _now: Time, amount: Int) : (Int, Int)
  (state + amount, state)
end

fn snapshot(state: Int, _now: Time) : List(Int)
  [state]
end

fn reject(_state: Int, _now: Time) : Result((Int, Int), Text)
  Error("application rejection")
end

fn defect(_state: Int, _now: Time) : (Int, Int)
  stop "tool defect"
end

fn busy(state: Int, _now: Time) : (Int, Int)
  while true
    Unit
  end
  (state, state)
end

fn helper(sim: Simulation) : Result((Int, Int), SimulationError)
  examples
    helper(Simulation.start(Main, t0)) == Ok((3, 0))
  end

  Simulation.send(sim, Main.add, 3)
end

t0 = Time.parse("2026-01-01T00:00:00Z") or stop "time"

test "a simulation can pass through a helper"
  sim = Simulation.start(Main, t0)
  assert helper(sim) == Ok((3, 0))
  Controls.reset(sim)
  assert helper(sim) == Ok((6, 3))
end

test "aliases see the same lifecycle and state; repeated crash is harmless"
  sim = Simulation.start(Main, t0)
  alias = sim
  assert helper(alias) == Ok((3, 0))
  Simulation.crash(sim)
  Simulation.crash(alias)
  assert helper(alias) == Error(ServiceDown)
  Simulation.advance(alias, 5.minutes)
  Simulation.restart(alias)
  assert helper(sim) == Ok((6, 3))
end

test "application Error and read-only results do not consume failure windows"
  sim = Simulation.start(Main, t0)
  assert helper(sim) == Ok((3, 0))
  for _i in 0..20
    assert Simulation.send(sim, Main.reject) == Ok(Error("application rejection"))
    assert Simulation.send(sim, Main.snapshot) == Ok([3])
  end
  Simulation.crash(sim)
  Simulation.restart(sim)
  assert Simulation.send(sim, Main.snapshot) == Ok([3])
end

test "D308 keeps state at internal restart and explicit restart resets its windows"
  sim = Simulation.start(Main, t0)
  assert helper(sim) == Ok((3, 0))
  for _i in 0..5
    assert Simulation.send(sim, Main.defect) == Error(ToolFailed)
  end
  assert Simulation.send(sim, Main.snapshot) == Ok([3])
  for _i in 0..10
    assert Simulation.send(sim, Main.defect) == Error(ToolFailed)
  end
  assert helper(sim) == Error(ServiceDown)
  Simulation.crash(sim)
  Simulation.crash(sim)
  Simulation.restart(sim)
  for _i in 0..5
    assert Simulation.send(sim, Main.defect) == Error(ToolFailed)
  end
  assert helper(sim) == Ok((6, 3))
end

test "work failure leaves the committed state unchanged"
  sim = Simulation.start(Main, t0)
  assert helper(sim) == Ok((3, 0))
  assert Simulation.send(sim, Main.busy) == Error(ToolFailed)
  assert Simulation.send(sim, Main.snapshot) == Ok([3])
end

test "recovery crosses the snapshot boundary"
  sim = Simulation.start(Main, t0)
  for i in 0..103
    assert Simulation.send(sim, Main.add, 7) == Ok(((i + 1) * 7, i * 7))
  end
  Simulation.crash(sim)
  Simulation.restart(sim)
  assert Simulation.send(sim, Main.snapshot) == Ok([721])
end

test "every deadline runs at its time, with source-order ties and before send"
  sim = Simulation.start(Ticks, t0)
  assert Simulation.send(sim, Ticks.history) == Ok([(t0, 1), (t0, 2)])
  Simulation.advance(sim, 0.seconds)
  assert Simulation.send(sim, Ticks.history) == Ok([(t0, 1), (t0, 2)])
  Simulation.advance(sim, 31.minutes)
  var expected = [(t0, 1), (t0, 2)]
  for i in 1..32
    expected = expected.append((t0 + i.minutes, 1))
    if i % 2 == 0
      expected = expected.append((t0 + i.minutes, 2))
    end
  end
  assert Client.history(sim) == Ok(expected)
  Simulation.crash(sim)
  Simulation.advance(sim, 2.minutes)
  Simulation.restart(sim)
  expected = expected.append((t0 + 33.minutes, 1)).append((t0 + 33.minutes, 2))
  assert Simulation.send(sim, Ticks.history) == Ok(expected)
end

test "ordinary runs in a simulation program still compare with Lean"
  assert 2 + 3 == 5
end

fn main() : Int
  0
end
