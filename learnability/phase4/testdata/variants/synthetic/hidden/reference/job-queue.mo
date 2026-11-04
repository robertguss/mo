## A small job queue that the Phase 4 fixture and grading scripts are tested on.
## A job is leased for 30 minutes at a time and tried at most three times.
module JobQueue
expose Queue,
Job(..),
JobState(..),
QueueError(..),
empty,
submit,
lease,
renew,
finish,
give_back,
expire,
jobs

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
end

empty = Queue(jobs: {}, next_id: 1)

fn submit(queue: Queue, payload: Text) : (Queue, Int)
  id = queue.next_id
  var after = queue
  after.jobs = after.jobs.set(id, Job(id: id, payload: payload, attempts: 0, state: Ready))
  after.next_id += 1
  (after, id)
end

fn lease(queue: Queue, now: Time) : (Queue, Option(Job))
  case queue.jobs.values.find(fn(job) job.state == Ready end)
    None: (queue, None)
    Some(job):
      var taken = job
      taken.state = Leased(until: now + 30.minutes)
      taken.attempts += 1
      (put(queue, taken), Some(taken))
  end
end

fn renew(queue: Queue, now: Time, id: Int, attempt: Int) : Result(Queue, QueueError)
  job = try held(queue, now, id, attempt)
  var renewed = job
  renewed.state = Leased(until: now + 30.minutes)
  Ok(put(queue, renewed))
end

fn finish(queue: Queue, now: Time, id: Int, attempt: Int) : Result(Queue, QueueError)
  job = try held(queue, now, id, attempt)
  var finished = job
  finished.state = Done
  Ok(put(queue, finished))
end

fn give_back(queue: Queue, now: Time, id: Int, attempt: Int) : Result(Queue, QueueError)
  job = try held(queue, now, id, attempt)
  Ok(release(queue, job))
end

fn expire(queue: Queue, now: Time) : Queue
  examples
    expire(empty, t0) == empty
  end

  var after = queue
  for job in queue.jobs.values
    if job.state is Leased(until: until) and until <= now
      after = release(after, job)
    end
  end
  after
end

fn jobs(queue: Queue) : List(Job)
  queue.jobs.values
end

fn held(queue: Queue, now: Time, id: Int, attempt: Int) : Result(Job, QueueError)
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

t0 = Time.parse("2026-01-01T00:00:00Z") or stop "the time is written correctly"

test "a job given back three times is dead"
  (submitted, id) = submit(empty, "try")
  var queue = submitted
  for attempt in 1..4
    assert lease(queue, t0) is (taken, Some(job))
    assert job.attempts == attempt
    assert give_back(taken, t0, id, attempt) is Ok(after)
    queue = after
  end
  assert queue.jobs.get(id) is Some(final) and final.state == Dead
end
