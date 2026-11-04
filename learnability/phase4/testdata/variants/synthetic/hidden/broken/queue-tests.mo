## The small queue's own simulation tests. None counts a dead job, so they pass
## despite the deliberate defect in `stats`.
module QueueTests
use Main{JobFilter, Page, Stats}
use JobQueue{Job}

t0 = Time.parse("2026-01-01T00:00:00Z") or stop "time"

fn submit(sim: Simulation, payload: Text) : Int
  return id if Simulation.send(sim, Main.submit, payload) is Ok(Ok((_, id)))
  stop "submit was refused"
end

fn lease(sim: Simulation) : Job
  return job if Simulation.send(sim, Main.lease) is Ok(Ok((_, Some(job))))
  stop "nothing was leased"
end

fn page(sim: Simulation, filter: JobFilter) : Page
  return found if Simulation.send(sim, Main.list, filter, 0, 100) is Ok(Ok(found))
  stop "list was refused"
end

test "a job goes from submitted to done, and stats counts it"
  sim = Simulation.start(Main, t0)
  id = submit(sim, "write the runbook")
  taken = lease(sim)
  assert taken.id == id
  assert Simulation.send(sim, Main.acknowledge, id, 1) is Ok(Ok(_))
  assert page(sim, Done).jobs.map(fn(job) job.id end) == [id]
  assert Simulation.send(sim, Main.stats) ==
    Ok(Stats(total: 1, ready: 0, leased: 0, done: 1, dead: 0, limit: 5000, now: t0))
end

test "three failed attempts make a job dead"
  sim = Simulation.start(Main, t0)
  id = submit(sim, "broken")
  for attempt in 1..4
    assert lease(sim).attempts == attempt
    assert Simulation.send(sim, Main.fail, id, attempt) is Ok(Ok(_))
  end
  assert page(sim, Dead).jobs.map(fn(job) job.id end) == [id]
end
