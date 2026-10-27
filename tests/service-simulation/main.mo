module Main
use TaskQueue
use JobQueue

t0 = Time.parse("2026-01-01T00:00:00Z") or stop "time"

test "an accepted job survives a crash through real log recovery"
  sim = Simulation.start(TaskQueue, t0)
  accepted = Simulation.send(sim, TaskQueue.submit, "deliver invoice")
  assert accepted is Ok((_, 1))
  Simulation.crash(sim)
  Simulation.restart(sim)
  assert Simulation.send(sim, TaskQueue.lease, 30.seconds) is Ok(Ok((_, Some(job)))) and
    job.id == 1 and
    job.payload == "deliver invoice" and
    job.attempts == 1 and
    job.state == JobQueue.Leased(until: t0 + 30.seconds)
end

test "ignoring an accepted reply and retrying after recovery submits a second job"
  sim = Simulation.start(TaskQueue, t0)
  _ignored = Simulation.send(sim, TaskQueue.submit, "once?")
  Simulation.crash(sim)
  Simulation.restart(sim)
  assert Simulation.send(sim, TaskQueue.submit, "once?") is Ok((_, 2))
  for id in 1..3
    assert Simulation.send(sim, TaskQueue.lease, 30.seconds) is Ok(Ok((_, Some(job)))) and
      job.id == id and
      job.payload == "once?" and
      job.attempts == 1
  end
  assert Simulation.send(sim, TaskQueue.lease, 30.seconds) is Ok(Ok((_, None)))
end

fn main() : Int
  0
end
