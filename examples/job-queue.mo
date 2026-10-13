## The core of a job queue. Jobs are submitted, leased to a worker for a while,
## and acknowledged when done. A lease that fails or expires puts its job back
## in the queue, up to three attempts in all; after the third, the job is dead.
## The queue is a value: every operation returns the new queue, and no
## operation ever loses a job.
module JobQueue
expose Queue, Job(..), JobState(..), QueueError(..), empty, submit, lease, acknowledge, fail, expire

max_attempts = 3

struct Job
  id: Int
  payload: Text
  attempts: Int
  state: JobState
end

enum JobState
  Ready
  Leased(until: Time)
  Done
  Dead
end

struct Queue
  jobs: Map(Int, Job)
  next_id: Int
end

enum QueueError
  NoSuchJob(id: Int)
  NotLeased(id: Int)
  BadLease(length: Duration)
end

empty = Queue(jobs: {}, next_id: 1)

fn submit(queue: Queue, payload: Text) : (Queue, Int)
  promise "a submitted job is in the queue, ready"
    result.0.jobs.get(result.1) is Some(job) and job.state == Ready
    allows queue: empty, payload: "x", result: (holding([ready(1)]), 1)
    forbids queue: empty, payload: "x", result: (empty, 1)
  end

  id = queue.next_id
  var after = queue
  after.jobs = after.jobs.set(id, Job(id: id, payload: payload, attempts: 0, state: Ready))
  after.next_id += 1
  (after, id)
end

## Leases the oldest ready job until now + length, counting an attempt. The
## length must be positive, and the lease must end by the year 9999.
fn lease(queue: Queue, now: Time, length: Duration) : Result((Queue, Option(Job)), QueueError)
  promise "a lease never loses a job"
    result is Ok((after, _)) implies queue.jobs.keys.all?(fn(id) after.jobs.has?(id) end)
    allows queue: holding([ready(1)]), now: t0, length: 30.seconds, result: Ok((
      holding([leased(1, t0 + 30.seconds)]),
      Some(leased(1, t0 + 30.seconds))
    ))
    forbids queue: holding([ready(1)]), now: t0, length: 30.seconds, result: Ok((empty, None))
  end

  examples
    lease(empty, t0, 30.seconds) == Ok((empty, None))
    lease(empty, t0, 0.seconds) == Error(BadLease(length: 0.seconds))
  end

  return Error(BadLease(length: length)) if length <= 0.seconds
  until = try now.checked_add(length).or_fail(BadLease(length: length))
  case queue.jobs.values.find(fn(job) job.state == Ready end)
    None: Ok((queue, None))
    Some(job):
      var taken = job
      taken.state = Leased(until: until)
      taken.attempts += 1
      Ok((put(queue, taken), Some(taken)))
  end
end

fn acknowledge(queue: Queue, id: Int) : Result(Queue, QueueError)
  promise "acknowledging finishes the job and keeps every job"
    result is Ok(after) implies queue.jobs.keys.all?(fn(other) after.jobs.has?(other) end)
    result is Ok(after) implies after.jobs.get(id) is Some(job) and job.state == Done
    allows queue: holding([leased(1, t0), ready(2)]), id: 1, result: Ok(
      holding([done(1), ready(2)])
    )
    forbids queue: holding([leased(1, t0), ready(2)]), id: 1, result: Ok(holding([done(1)]))
  end

  job = try leased_job(queue, id)
  var finished = job
  finished.state = Done
  Ok(put(queue, finished))
end

## Gives a leased job back after its worker failed: ready again, or dead after
## its last attempt.
fn fail(queue: Queue, id: Int) : Result(Queue, QueueError)
  promise "a job is attempted at most three times"
    result is Ok(after) and after.jobs.get(id) is Some(job) and job.attempts >= max_attempts implies
      job.state == Dead
    allows queue: holding([tried(1, 3)]), id: 1, result: Ok(holding([dead(1)]))
    forbids queue: holding([tried(1, 3)]), id: 1, result: Ok(holding([retried(1, 3)]))
  end

  examples
    fail(holding([tried(1, 1)]), 1) == Ok(holding([retried(1, 1)]))
    fail(holding([tried(1, 3)]), 1) == Ok(holding([dead(1)]))
    fail(holding([ready(1)]), 1) == Error(NotLeased(id: 1))
    fail(empty, 9) == Error(NoSuchJob(id: 9))
  end

  job = try leased_job(queue, id)
  Ok(release(queue, job))
end

## Gives back every job whose lease ran out by now, as if its worker failed.
fn expire(queue: Queue, now: Time) : Queue
  examples
    expire(holding([leased(1, t0)]), t0) == holding([retried(1, 1)])
    expire(holding([leased(1, t0 + 1.seconds)]), t0) == holding([leased(1, t0 + 1.seconds)])
  end

  var after = queue
  for job in queue.jobs.values
    if job.state is Leased(until: until) and until <= now
      after = release(after, job)
    end
  end
  after
end

fn leased_job(queue: Queue, id: Int) : Result(Job, QueueError)
  job = try queue.jobs.get(id).or_fail(NoSuchJob(id: id))
  case job.state
    Leased(until: _): Ok(job)
    Ready | Done | Dead: Error(NotLeased(id: id))
  end
end

fn release(queue: Queue, job: Job) : Queue
  var released = job
  released.state = if job.attempts >= max_attempts: Dead else: Ready
  put(queue, released)
end

fn put(queue: Queue, job: Job) : Queue
  var after = queue
  after.jobs = after.jobs.set(job.id, job)
  after
end

t0 = Time.parse("2026-10-12T09:00:00Z") or stop "the time is written correctly"

fn holding(jobs: List(Job)) : Queue
  Queue(jobs: jobs.map(fn(job) (job.id, job) end).to_map, next_id: jobs.size + 1)
end

fn ready(id: Int) : Job
  Job(id: id, payload: "x", attempts: 0, state: Ready)
end

fn leased(id: Int, until: Time) : Job
  Job(id: id, payload: "x", attempts: 1, state: Leased(until: until))
end

fn tried(id: Int, attempts: Int) : Job
  Job(id: id, payload: "x", attempts: attempts, state: Leased(until: t0))
end

fn retried(id: Int, attempts: Int) : Job
  Job(id: id, payload: "x", attempts: attempts, state: Ready)
end

fn done(id: Int) : Job
  Job(id: id, payload: "x", attempts: 1, state: Done)
end

fn dead(id: Int) : Job
  Job(id: id, payload: "x", attempts: 3, state: Dead)
end

test "a job that keeps failing is tried three times, then dead"
  (submitted, id) = submit(empty, "send the invoice")
  var queue = submitted
  for attempt in 1..4
    assert lease(queue, t0, 30.seconds) is Ok((leased_queue, Some(job)))
    assert job.attempts == attempt
    assert fail(leased_queue, id) is Ok(after)
    queue = after
  end
  assert queue.jobs.get(id) is Some(final)
  assert final.state == Dead
  assert lease(queue, t0, 30.seconds) == Ok((queue, None))
end
