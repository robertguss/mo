## The core of a job queue. Jobs are submitted, leased to a worker for a while,
## and acknowledged when done. A lease that fails or expires puts its job back
## in the queue, up to three attempts in all; after the third, the job is dead.
## The queue is a value: every operation returns the new queue, and no
## operation ever loses a job.
module JobQueue
expose Queue,
Job(..),
JobState(..),
QueueError(..),
empty,
submit,
lease,
heartbeat,
acknowledge,
fail,
expire

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

## Renews the current lease until now + 30 minutes without counting an attempt.
## NotLeased also covers an expired lease or a different attempt.
fn heartbeat(queue: Queue, now: Time, id: Int, attempt: Int) : Result(Queue, QueueError)
  promise "only a current lease can be renewed"
    result is Ok(_) implies
      queue.jobs.get(id) is Some(job) and
        job.attempts == attempt and
        job.state is Leased(until: until) and
        until > now
    allows queue: holding([leased(1, t0 + 1.seconds)]), now: t0, id: 1, attempt: 1, result: Ok(
      holding([leased(1, t0 + 30.minutes)])
    )
    forbids queue: holding([leased(1, t0 + 1.seconds)]), now: t0, id: 1, attempt: 2, result: Ok(
      holding([leased(1, t0 + 30.minutes)])
    )
  end

  promise "renewing a lease keeps every job and its attempt"
    result is Ok(after) implies queue.jobs.keys.all?(fn(other) after.jobs.has?(other) end)
    result is Ok(after) implies
      after.jobs.get(id) is Some(job) and
        job.attempts == attempt and
        job.state is Leased(until: until) and
        now.checked_add(30.minutes) == Some(until)
    allows queue: holding([leased(1, t0 + 1.seconds), ready(2)]), now: t0, id: 1, attempt: 1, result: Ok(
      holding([leased(1, t0 + 30.minutes), ready(2)])
    )
    forbids queue: holding([leased(1, t0 + 1.seconds), ready(2)]), now: t0, id: 1, attempt: 1, result: Ok(
      holding([leased(1, t0 + 30.minutes)])
    )
  end

  job = try leased_job(queue, now, id, attempt)
  until = try now.checked_add(30.minutes).or_fail(BadLease(length: 30.minutes))
  var renewed = job
  renewed.state = Leased(until: until)
  Ok(put(queue, renewed))
end

## Finishes a current lease. Expired or mismatched attempts return NotLeased.
fn acknowledge(queue: Queue, now: Time, id: Int, attempt: Int) : Result(Queue, QueueError)
  promise "only a current lease can be acknowledged"
    result is Ok(_) implies
      queue.jobs.get(id) is Some(job) and
        job.attempts == attempt and
        job.state is Leased(until: until) and
        until > now
    allows queue: holding([leased(1, t0 + 30.seconds)]), now: t0, id: 1, attempt: 1, result: Ok(
      holding([done(1)])
    )
    forbids queue: holding([leased(1, t0 + 30.seconds)]), now: t0, id: 1, attempt: 2, result: Ok(
      holding([done(1)])
    )
  end

  promise "acknowledging finishes the job and keeps every job"
    result is Ok(after) implies queue.jobs.keys.all?(fn(other) after.jobs.has?(other) end)
    result is Ok(after) implies after.jobs.get(id) is Some(job) and job.state == Done
    allows queue: holding([leased(1, t0 + 30.seconds), ready(2)]), now: t0, id: 1, attempt: 1, result: Ok(
      holding([done(1), ready(2)])
    )
    forbids queue: holding([leased(1, t0 + 30.seconds), ready(2)]), now: t0, id: 1, attempt: 1, result: Ok(
      holding([done(1)])
    )
  end

  job = try leased_job(queue, now, id, attempt)
  var finished = job
  finished.state = Done
  Ok(put(queue, finished))
end

## Gives a current lease back after its worker failed: ready again, or dead after
## its last attempt. Expired or mismatched attempts return NotLeased.
fn fail(queue: Queue, now: Time, id: Int, attempt: Int) : Result(Queue, QueueError)
  promise "only a current lease can fail"
    result is Ok(_) implies
      queue.jobs.get(id) is Some(job) and
        job.attempts == attempt and
        job.state is Leased(until: until) and
        until > now
    allows queue: holding([tried(1, 2)]), now: t0, id: 1, attempt: 2, result: Ok(
      holding([retried(1, 2)])
    )
    forbids queue: holding([tried(1, 2)]), now: t0, id: 1, attempt: 1, result: Ok(
      holding([retried(1, 2)])
    )
  end

  promise "a job is attempted at most three times"
    result is Ok(after) and after.jobs.get(id) is Some(job) and job.attempts >= max_attempts implies
      job.state == Dead
    allows queue: holding([tried(1, 3)]), now: t0, id: 1, attempt: 3, result: Ok(holding([dead(1)]))
    forbids queue: holding([tried(1, 3)]), now: t0, id: 1, attempt: 3, result: Ok(
      holding([retried(1, 3)])
    )
  end

  examples
    fail(holding([tried(1, 1)]), t0, 1, 1) == Ok(holding([retried(1, 1)]))
    fail(holding([tried(1, 3)]), t0, 1, 3) == Ok(holding([dead(1)]))
    fail(holding([ready(1)]), t0, 1, 0) == Error(NotLeased(id: 1))
    fail(empty, t0, 9, 1) == Error(NoSuchJob(id: 9))
  end

  job = try leased_job(queue, now, id, attempt)
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

fn leased_job(queue: Queue, now: Time, id: Int, attempt: Int) : Result(Job, QueueError)
  job = try queue.jobs.get(id).or_fail(NoSuchJob(id: id))
  case job.state
    Leased(until: until): if job.attempts == attempt and until > now: Ok(job) else: Error(
      NotLeased(id: id)
    )
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
  Job(id: id, payload: "x", attempts: attempts, state: Leased(until: t0 + 30.seconds))
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
    assert fail(leased_queue, t0, id, attempt) is Ok(after)
    queue = after
  end
  assert queue.jobs.get(id) is Some(final)
  assert final.state == Dead
  assert lease(queue, t0, 30.seconds) == Ok((queue, None))
end

test "heartbeat renews from now without counting an attempt or changing another job"
  (first, id) = submit(empty, "send the invoice")
  (submitted, other) = submit(first, "publish the report")
  assert lease(submitted, t0, 30.minutes) is Ok((taken, Some(job)))
  now = t0 + 20.minutes
  assert heartbeat(taken, now, id, job.attempts) is Ok(renewed)
  assert renewed.jobs.get(id) is Some(current)
  assert current ==
    Job(id: id, payload: "send the invoice", attempts: 1, state: Leased(until: t0 + 50.minutes))
  assert renewed.jobs.get(other) == submitted.jobs.get(other)
  assert renewed.next_id == submitted.next_id
  assert expire(renewed, t0 + 30.minutes) == renewed
  assert acknowledge(renewed, t0 + 50.minutes - 1.seconds, id, 1) is Ok(finished)
  assert finished.jobs.get(id) is Some(completed) and completed.state == Done
  assert expire(renewed, t0 + 50.minutes).jobs.get(id) is Some(returned) and returned.state == Ready
  late = Time.parse("9999-12-31T23:40:00Z") or stop "the time is written correctly"
  queue = holding([leased(1, late + 1.seconds)])
  assert heartbeat(queue, late, 1, 1) == Error(BadLease(length: 30.minutes))
end

test "old attempts cannot change a newer lease"
  (submitted, id) = submit(empty, "send the invoice")
  assert lease(submitted, t0, 30.seconds) is Ok((first, Some(job)))
  returned = expire(first, t0 + 30.seconds)
  now = t0 + 31.seconds
  assert lease(returned, now, 30.seconds) is Ok((second, Some(current)))
  assert current.attempts == 2
  for attempt in [0, job.attempts, 3]
    assert heartbeat(second, now, id, attempt) == Error(NotLeased(id: id))
    assert acknowledge(second, now, id, attempt) == Error(NotLeased(id: id))
    assert fail(second, now, id, attempt) == Error(NotLeased(id: id))
  end
  assert heartbeat(second, now, id, current.attempts) is Ok(renewed)
  assert renewed.jobs.get(id) is Some(extended) and extended.attempts == 2
  assert acknowledge(second, now, id, current.attempts) is Ok(finished)
  assert finished.jobs.get(id) is Some(completed) and completed.state == Done
  assert fail(second, now, id, current.attempts) is Ok(released)
  assert released.jobs.get(id) is Some(retry) and retry.state == Ready and retry.attempts == 2
  queue = holding([leased(1, t0 + 30.seconds), ready(2)])
  assert acknowledge(queue, t0, 1, 1) == Ok(holding([done(1), ready(2)]))
  assert fail(queue, t0, 1, 1) == Ok(holding([retried(1, 1), ready(2)]))
end

test "expired and ended leases are refused even before the expiry tick"
  for job in [leased(1, t0), leased(1, t0 - 1.seconds), ready(1), done(1), dead(1)]
    queue = holding([job])
    assert heartbeat(queue, t0, 1, job.attempts) == Error(NotLeased(id: 1))
    assert acknowledge(queue, t0, 1, job.attempts) == Error(NotLeased(id: 1))
    assert fail(queue, t0, 1, job.attempts) == Error(NotLeased(id: 1))
  end
  assert heartbeat(empty, t0, 9, 1) == Error(NoSuchJob(id: 9))
  assert acknowledge(empty, t0, 9, 1) == Error(NoSuchJob(id: 9))
end
