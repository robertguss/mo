## A value-based FIFO job queue. Completed and dead jobs remain in its ledger.
## Lease tokens prevent an expired worker from acknowledging a later attempt.
module JobQueue
expose Queue, Job(..), JobState(..), Lease(..), QueueError(..), empty, jobs, submit, lease, acknowledge, fail, expire

enum JobState
  Ready
  Leased(token: BigInt, until: Time)
  Completed
  Dead
end

struct Job
  id: BigInt
  payload: Text
  attempts: Int
  state: JobState
end

struct Lease
  id: BigInt
  token: BigInt
  payload: Text
  attempt: Int
  until: Time
end

struct Queue
  jobs: List(Job)
  next_id: BigInt
  next_token: BigInt
end

enum QueueError
  InvalidDuration
  UnknownJob
  StaleLease
end

epoch = Time.parse("2026-01-01T00:00:00Z") or stop "the epoch is valid RFC 3339"
sample_queue = Queue(jobs: [Job(id: 1.to_big, payload: "work", attempts: 0, state: Ready)],
  next_id: 2.to_big, next_token: 1.to_big)

fn empty() : Queue
  Queue(jobs: [], next_id: 1.to_big, next_token: 1.to_big)
end

fn jobs(queue: Queue) : List(Job)
  queue.jobs
end

fn keeps_jobs?(before: Queue, after: Queue) : Bool
  before.jobs.all?(fn(old_job)
    after.jobs.any?(fn(new_job) new_job.id == old_job.id and new_job.payload == old_job.payload end)
  end)
end

fn submit(queue: Queue, payload: Text) : (Queue, BigInt)
  promise "submission retains every job and adds exactly one ready job"
    keeps_jobs?(queue, result.0)
    result.0.jobs.size == queue.jobs.size + 1
    result.1 == queue.next_id
    result.0.jobs.last == Some(Job(id: result.1, payload: payload, attempts: 0, state: Ready))
    allows queue: empty(), payload: "work", result: (sample_queue, 1.to_big)
    forbids queue: empty(), payload: "work", result: (empty(), 1.to_big)
  end

  var updated = queue
  updated.jobs = updated.jobs.append(Job(id: queue.next_id, payload: payload, attempts: 0, state: Ready))
  updated.next_id += 1
  (updated, queue.next_id)
end

fn expire(queue: Queue, now: Time) : Queue
  promise "expiring leases never loses a job or changes the number of jobs"
    keeps_jobs?(queue, result)
    result.jobs.size == queue.jobs.size
    allows queue: sample_queue, now: epoch, result: sample_queue
    forbids queue: sample_queue, now: epoch, result: empty()
  end

  var updated = queue
  updated.jobs = queue.jobs.map(fn(job)
    state = case job.state
      Leased(token: _, until: deadline):
        if now >= deadline: (if job.attempts >= 3: Dead else: Ready) else: job.state
      Ready | Completed | Dead: job.state
    end
    Job(id: job.id, payload: job.payload, attempts: job.attempts, state: state)
  end)
  updated
end

fn lease(queue: Queue, now: Time, duration: Duration) : (Queue, Result(Option(Lease), QueueError))
  promise "leasing retains every job and never creates or removes one"
    keeps_jobs?(queue, result.0)
    result.0.jobs.size == queue.jobs.size
    result.0.jobs.all?(fn(job) job.attempts >= 0 and job.attempts <= 3 end)
    allows queue: sample_queue, now: epoch, duration: 0.seconds, result: (sample_queue, Error(InvalidDuration))
    forbids queue: sample_queue, now: epoch, duration: 0.seconds, result: (empty(), Error(InvalidDuration))
  end

  return (queue, Error(InvalidDuration)) if duration <= 0.seconds
  deadline = case now.checked_add(duration)
    Some(value): value
    None: return (queue, Error(InvalidDuration))
  end
  var updated = expire(queue, now)
  for (index, job) in updated.jobs.enumerate
    continue if not (job.state is Ready)
    var leased_job = job
    leased_job.attempts += 1
    leased_job.state = Leased(token: updated.next_token, until: deadline)
    updated.jobs = case updated.jobs.set(index, leased_job)
      Some(value): value
      None: stop "enumeration gives an existing job index"
    end
    granted = Lease(id: job.id, token: updated.next_token, payload: job.payload,
      attempt: leased_job.attempts, until: deadline)
    updated.next_token += 1
    return (updated, Ok(Some(granted)))
  end
  (updated, Ok(None))
end

fn finish(queue: Queue, id: BigInt, token: BigInt, now: Time, success?: Bool) : (Queue, Result(Unit, QueueError))
  var updated = expire(queue, now)
  for (index, job) in updated.jobs.enumerate
    continue if job.id != id
    case job.state
      Leased(token: current_token, until: _):
        return (updated, Error(StaleLease)) if token != current_token
        var replacement = job
        replacement.state = if success?: Completed else if job.attempts >= 3: Dead else: Ready
        updated.jobs = case updated.jobs.set(index, replacement)
          Some(value): value
          None: stop "enumeration gives an existing job index"
        end
        return (updated, Ok(Unit))
      Ready | Completed | Dead: return (updated, Error(StaleLease))
    end
  end
  (updated, Error(UnknownJob))
end

fn acknowledge(queue: Queue, id: BigInt, token: BigInt, now: Time) : (Queue, Result(Unit, QueueError))
  promise "acknowledgment retains all jobs, including the completed job"
    keeps_jobs?(queue, result.0)
    result.0.jobs.size == queue.jobs.size
    allows queue: sample_queue, id: 99.to_big, token: 1.to_big, now: epoch, result: (sample_queue, Error(UnknownJob))
    forbids queue: sample_queue, id: 99.to_big, token: 1.to_big, now: epoch, result: (empty(), Error(UnknownJob))
  end

  finish(queue, id, token, now, true)
end

fn fail(queue: Queue, id: BigInt, token: BigInt, now: Time) : (Queue, Result(Unit, QueueError))
  promise "failure retains all jobs, including a job that becomes dead"
    keeps_jobs?(queue, result.0)
    result.0.jobs.size == queue.jobs.size
    allows queue: sample_queue, id: 99.to_big, token: 1.to_big, now: epoch, result: (sample_queue, Error(UnknownJob))
    forbids queue: sample_queue, id: 99.to_big, token: 1.to_big, now: epoch, result: (empty(), Error(UnknownJob))
  end

  finish(queue, id, token, now, false)
end

test "leasing is FIFO, acknowledgments retain jobs, and the queue is a value"
  (first, first_id) = submit(empty(), "first")
  (second, second_id) = submit(first, "second")
  (third, response) = lease(second, epoch, 10.seconds)
  assert response is Ok(Some(granted))
  assert granted.id == first_id
  assert second.jobs.first is Some(original)
  assert original.state == Ready
  (fourth, acknowledged) = acknowledge(third, first_id, granted.token, epoch)
  assert acknowledged == Ok(Unit)
  assert fourth.jobs.size == 2
  assert fourth.jobs.first is Some(completed)
  assert completed.state == Completed
  (fifth, next_response) = lease(fourth, epoch, 10.seconds)
  assert next_response is Ok(Some(next_lease))
  assert next_lease.id == second_id
  assert fifth.jobs.size == 2
end

test "expiration and failure allow at most three tries and reject old lease tokens"
  (first, id) = submit(empty(), "work")
  (second, response) = lease(first, epoch, 1.seconds)
  assert response is Ok(Some(initial_lease))
  (third, stale) = acknowledge(second, id, initial_lease.token, epoch + 1.seconds)
  assert stale == Error(StaleLease)
  (fourth, retry) = lease(third, epoch + 1.seconds, 1.seconds)
  assert retry is Ok(Some(second_lease))
  assert second_lease.attempt == 2
  (fifth, old_ack) = acknowledge(fourth, id, initial_lease.token, epoch + 1.seconds)
  assert old_ack == Error(StaleLease)
  (sixth, failed) = fail(fifth, id, second_lease.token, epoch + 1.seconds)
  assert failed == Ok(Unit)
  (seventh, final_response) = lease(sixth, epoch + 1.seconds, 1.seconds)
  assert final_response is Ok(Some(final_lease))
  assert final_lease.attempt == 3
  eighth = expire(seventh, epoch + 2.seconds)
  assert eighth.jobs.size == 1
  assert eighth.jobs.first is Some(dead_job)
  assert dead_job.state == Dead
  (ninth, unavailable) = lease(eighth, epoch + 2.seconds, 1.seconds)
  assert unavailable == Ok(None)
  assert ninth.jobs.size == 1
end
