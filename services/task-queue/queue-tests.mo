## Simulation tests for the task queue. They live apart from the service so that
## editing a test cannot change the program that is served.
module QueueTests
use Main{JobFilter, Page, PageError, SubmitError}
use JobQueue{Job, QueueError}

t0 = Time.parse("2026-01-01T00:00:00Z") or stop "time"
filters = [(AllJobs, 5), (Ready, 2), (Leased, 1), (Done, 1), (Dead, 1)]

fn submit(sim: Simulation, payload: Text) : Int
  return id if Simulation.send(sim, Main.submit, payload) is Ok(Ok((_, id)))
  stop "submit was refused"
end

fn lease(sim: Simulation) : Job
  return job if Simulation.send(sim, Main.lease) is Ok(Ok((_, Some(job))))
  stop "nothing was leased"
end

fn page(sim: Simulation, filter: JobFilter, after: Int, limit: Int) : Page
  return found if Simulation.send(sim, Main.list, filter, after, limit) is Ok(Ok(found))
  stop "list was refused"
end

fn job(sim: Simulation, id: Int) : Job
  page(sim, AllJobs, id - 1, 1).jobs.first or stop "no such job"
end

fn acknowledged?(sim: Simulation, job: Job) : Bool
  Simulation.send(sim, Main.acknowledge, job.id, job.attempts) is Ok(Ok(_))
end

fn fill(size: Int) : Text
  "x".repeat(size)
end

test "a job goes from submitted to done"
  sim = Simulation.start(Main, t0)
  id = submit(sim, "write the runbook")
  assert job(sim, id).state == JobQueue.Ready
  taken = lease(sim)
  assert taken ==
    Job(
      id: id,
      payload: "write the runbook",
      attempts: 1,
      state: JobQueue.Leased(until: t0 + 30.minutes)
    )
  assert Simulation.send(sim, Main.heartbeat, id, 1) is Ok(Ok(_))
  assert acknowledged?(sim, taken)
  assert job(sim, id).state == JobQueue.Done
  assert job(sim, id).attempts == 1
end

test "leasing an empty queue gives nothing and leaves the queue as it was"
  sim = Simulation.start(Main, t0)
  assert Simulation.send(sim, Main.lease) is Ok(Ok((_, None)))
  assert page(sim, AllJobs, 0, 100).jobs == []
  id = submit(sim, "later")
  assert lease(sim).id == id
end

test "a heartbeat carries the lease past its first end, and it still ends"
  sim = Simulation.start(Main, t0)
  id = submit(sim, "long task")
  taken = lease(sim)
  Simulation.advance(sim, 29.minutes)
  assert Simulation.send(sim, Main.heartbeat, id, 1) is Ok(Ok(_))
  Simulation.advance(sim, 29.minutes)
  assert job(sim, id).state == JobQueue.Leased(until: t0 + 59.minutes)
  assert job(sim, id).attempts == 1
  Simulation.advance(sim, 1.minute)
  assert Simulation.send(sim, Main.acknowledge, id, 1) == Ok(Error(NotLeased(id: id)))
  assert job(sim, id).state == JobQueue.Ready
  assert taken.attempts == job(sim, id).attempts
end

test "three failed attempts make a job dead, and it is never leased again"
  sim = Simulation.start(Main, t0)
  id = submit(sim, "broken")
  for attempt in 1..4
    assert lease(sim).attempts == attempt
    assert Simulation.send(sim, Main.fail, id, attempt) is Ok(Ok(_))
  end
  assert job(sim, id).state == JobQueue.Dead
  assert job(sim, id).attempts == 3
  assert Simulation.send(sim, Main.lease) is Ok(Ok((_, None)))
end

test "three expired leases make a job dead through the timer alone"
  sim = Simulation.start(Main, t0)
  id = submit(sim, "abandoned")
  for attempt in 1..4
    assert lease(sim).attempts == attempt
    Simulation.advance(sim, 30.minutes)
  end
  assert job(sim, id).state == JobQueue.Dead
  assert job(sim, id).attempts == 3
end

test "a dead job's payload can be submitted again as a new job, and the record stays"
  sim = Simulation.start(Main, t0)
  id = submit(sim, "try again")
  for attempt in 1..4
    assert lease(sim).attempts == attempt
    assert Simulation.send(sim, Main.fail, id, attempt) is Ok(Ok(_))
  end
  dead = page(sim, Dead, 0, 100).jobs
  assert dead.size == 1
  record = dead.first or stop "no dead job"
  again = submit(sim, record.payload)
  assert again != id
  assert lease(sim).payload == "try again"
  assert page(sim, Dead, 0, 100).jobs == dead
end

test "a repeated acknowledge reads as done"
  sim = Simulation.start(Main, t0)
  id = submit(sim, "lost reply")
  taken = lease(sim)
  assert acknowledged?(sim, taken)
  assert Simulation.send(sim, Main.acknowledge, id, 1) == Ok(Error(NotLeased(id: id)))
  assert job(sim, id).state == JobQueue.Done
  assert job(sim, id).attempts == taken.attempts
end

test "a late acknowledge reads as a lease that ran out"
  sim = Simulation.start(Main, t0)
  id = submit(sim, "slow")
  taken = lease(sim)
  Simulation.advance(sim, 31.minutes)
  assert Simulation.send(sim, Main.acknowledge, id, 1) == Ok(Error(NotLeased(id: id)))
  assert job(sim, id).state == JobQueue.Ready
  assert job(sim, id).attempts == taken.attempts
end

test "a late worker sees that a newer attempt owns the job"
  sim = Simulation.start(Main, t0)
  id = submit(sim, "shared")
  first = lease(sim)
  Simulation.advance(sim, 31.minutes)
  second = lease(sim)
  assert Simulation.send(sim, Main.acknowledge, id, first.attempts) == Ok(Error(NotLeased(id: id)))
  assert job(sim, id).attempts > first.attempts
  assert acknowledged?(sim, second)
end

test "a last attempt that expires reads as dead"
  sim = Simulation.start(Main, t0)
  id = submit(sim, "last")
  for _attempt in 1..3
    assert lease(sim).id == id
    Simulation.advance(sim, 30.minutes)
  end
  taken = lease(sim)
  Simulation.advance(sim, 31.minutes)
  assert Simulation.send(sim, Main.acknowledge, id, 3) == Ok(Error(NotLeased(id: id)))
  assert job(sim, id).state == JobQueue.Dead
  assert job(sim, id).attempts == taken.attempts
end

test "a payload of exactly the limit is accepted and one byte more is refused"
  sim = Simulation.start(Main, t0)
  id = submit(sim, fill(2048))
  assert Simulation.send(sim, Main.submit, fill(2049)) ==
    Ok(Error(PayloadTooLarge(limit: 2048, bytes: 2049)))
  assert page(sim, AllJobs, 0, 100).jobs.size == 1
  assert lease(sim).payload == fill(2048)
  assert id == 1
end

test "paging reads every job once, in order"
  sim = Simulation.start(Main, t0)
  for i in 1..26
    assert submit(sim, "job #{i}") == i
  end
  var seen = []
  var after = 0
  var pages = 0
  while true
    found = page(sim, AllJobs, after, 10)
    seen = seen.concat(found.jobs.map(fn(item) item.id end))
    pages += 1
    case found.next
      Some(next):
        assert found.jobs.size == 10
        after = next
      None: break
    end
  end
  assert seen == (1..26).to_list
  assert pages == 3
  assert page(sim, AllJobs, 25, 10).jobs == []
  assert page(sim, AllJobs, 25, 10).next == None
end

test "an exact last page has no next page"
  sim = Simulation.start(Main, t0)
  for i in 1..11
    assert submit(sim, "job #{i}") == i
  end
  assert page(sim, AllJobs, 0, 10).next == None
  assert page(sim, AllJobs, 0, 9).next == Some(9)
end

test "each filter lists only its own jobs, and paging skips the rest"
  sim = Simulation.start(Main, t0)
  dead = submit(sim, "dead")
  for attempt in 1..4
    assert lease(sim).attempts == attempt
    assert Simulation.send(sim, Main.fail, dead, attempt) is Ok(Ok(_))
  end
  done = submit(sim, "done")
  assert acknowledged?(sim, lease(sim))
  leased = submit(sim, "leased")
  assert lease(sim).id == leased
  first = submit(sim, "ready")
  second = submit(sim, "ready too")
  for (filter, size) in filters
    assert page(sim, filter, 0, 100).jobs.size == size
  end
  assert page(sim, Dead, 0, 100).jobs.map(fn(item) item.id end) == [dead]
  assert page(sim, Done, 0, 100).jobs.map(fn(item) item.id end) == [done]
  assert page(sim, Leased, 0, 100).jobs.map(fn(item) item.id end) == [leased]
  assert page(sim, Ready, 0, 1).jobs.map(fn(item) item.id end) == [first]
  assert page(sim, Ready, 0, 1).next == Some(first)
  assert page(sim, Ready, first, 1).jobs.map(fn(item) item.id end) == [second]
  assert page(sim, Ready, first, 1).next == None
end

test "bad list arguments are refused without counting as failures"
  sim = Simulation.start(Main, t0)
  id = submit(sim, "still here")
  for _round in 1..6
    for (after, limit) in [(0, 0), (0, -1), (0, 101), (-1, 10)]
      assert Simulation.send(sim, Main.list, AllJobs, after, limit) ==
        Ok(Error(BadPage(after: after, limit: limit, max_limit: 100)))
    end
  end
  assert page(sim, AllJobs, 0, 100).jobs.size == 1
  assert page(sim, AllJobs, 0, 1).jobs.size == 1
  assert lease(sim).id == id
end

test "listing leaves the queue alone"
  sim = Simulation.start(Main, t0)
  first = submit(sim, "first")
  second = submit(sim, "second")
  before = page(sim, AllJobs, 0, 100).jobs
  assert page(sim, AllJobs, 0, 100).jobs == before
  assert lease(sim).id == first
  assert lease(sim).id == second
end

test "a crash keeps ready, leased, done and dead jobs"
  sim = Simulation.start(Main, t0)
  dead = submit(sim, "dead")
  for attempt in 1..4
    assert lease(sim).attempts == attempt
    assert Simulation.send(sim, Main.fail, dead, attempt) is Ok(Ok(_))
  end
  done = submit(sim, "done")
  assert acknowledged?(sim, lease(sim))
  leased = submit(sim, "leased")
  assert lease(sim).id == leased
  ready = submit(sim, "ready")
  before = page(sim, AllJobs, 0, 100).jobs
  Simulation.crash(sim)
  Simulation.restart(sim)
  assert page(sim, AllJobs, 0, 100).jobs == before
  assert job(sim, dead).state == JobQueue.Dead
  assert job(sim, done).state == JobQueue.Done
  assert Simulation.send(sim, Main.heartbeat, leased, 1) is Ok(Ok(_))
  assert lease(sim).id == ready
end

test "an outage past the lease's end loses the lease and spends no extra attempt"
  sim = Simulation.start(Main, t0)
  id = submit(sim, "interrupted")
  assert lease(sim).id == id
  Simulation.crash(sim)
  Simulation.advance(sim, 31.minutes)
  Simulation.restart(sim)
  assert Simulation.send(sim, Main.heartbeat, id, 1) == Ok(Error(NotLeased(id: id)))
  assert job(sim, id).state == JobQueue.Ready
  assert job(sim, id).attempts == 1
end

test "an outage past the last attempt's lease leaves the job dead"
  sim = Simulation.start(Main, t0)
  id = submit(sim, "interrupted twice over")
  for attempt in 1..3
    assert lease(sim).attempts == attempt
    assert Simulation.send(sim, Main.fail, id, attempt) is Ok(Ok(_))
  end
  assert lease(sim).attempts == 3
  Simulation.crash(sim)
  Simulation.advance(sim, 31.minutes)
  Simulation.restart(sim)
  assert Simulation.send(sim, Main.heartbeat, id, 3) == Ok(Error(NotLeased(id: id)))
  assert job(sim, id).state == JobQueue.Dead
end

test "a lease whose reply was lost comes back after 30 minutes"
  sim = Simulation.start(Main, t0)
  id = submit(sim, "lost")
  _ignored = Simulation.send(sim, Main.lease)
  Simulation.advance(sim, 30.minutes)
  assert job(sim, id).state == JobQueue.Ready
  assert job(sim, id).attempts == 1
  assert lease(sim).attempts == 2
end

test "a full queue refuses submit for good and everything else keeps working"
  sim = Simulation.start(Main, t0)
  for i in 1..5001
    assert submit(sim, "") == i
  end
  assert Simulation.send(sim, Main.submit, "one too many") is
    Ok(Error(QueueFull(limit: limit, advice: advice)))
  assert limit == 5000
  assert advice.contains?("cannot help")
  first = lease(sim)
  assert Simulation.send(sim, Main.heartbeat, first.id, 1) is Ok(Ok(_))
  assert acknowledged?(sim, first)
  second = lease(sim)
  assert Simulation.send(sim, Main.fail, second.id, 1) is Ok(Ok(_))
  third = lease(sim)
  Simulation.advance(sim, 30.minutes)
  assert job(sim, third.id).state == JobQueue.Ready
  assert page(sim, AllJobs, 4900, 100).jobs.size == 100
end
