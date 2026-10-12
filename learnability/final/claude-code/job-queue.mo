## The core of a job queue, as a value.
##
## Producers submit jobs. A worker leases the oldest ready job for a given
## length of time, does it, and then acknowledges it, which leaves it done, or
## fails it. A lease that runs out before either is expired, which counts as a
## failed try. A job is tried at most three times: after its third failed try
## it is dead, and it stays in the queue, for someone to look at, until
## `take_finished` hands it back with the done jobs.
##
## Every operation takes a queue and returns a new one; the old queue is left
## as it was. None of them loses a job: a job moves from ready to leased, and
## on to done, back to ready, or to dead, but it is never dropped, and a done
## or dead job never comes back. Each operation promises this, and `mo test`
## checks the promises on generated queues.
module JobQueue
expose Queue, Job(..), State(..), Lease(..), LeaseError(..), ReportError(..),
  max_attempts, empty, submit, lease, acknowledge, fail, expire, take_finished,
  find_job, jobs

## How many times a job is tried before it is dead.
max_attempts = 3

## A queue of jobs: every job submitted to it and not yet taken out, by id,
## oldest first, and the id the next job will get. Outside this module a queue
## comes only from `empty` and the operations below, so it keeps their rules:
## each job is stored under its own id, and ids are below `next_id`; a ready
## job has been tried fewer than three times, and a dead one three times.
struct Queue
  jobs: Map(Int, Job)
  next_id: Int
end

## A job as the queue holds it: what to run, how many times it has been
## leased, and where it is now.
struct Job
  id: Int
  payload: Text
  attempts: Int
  state: State
end

## Where a job is.
enum State
  Ready                # waiting for a worker
  Leased(until: Time)  # a worker has it until then
  Done                 # acknowledged: it worked
  Dead                 # its last try failed
end

## A job handed to a worker: what to run, which try this is, and when the
## lease runs out. The worker hands it back to `acknowledge` or `fail`. Each
## lease of a job has the next attempt number, so the number tells this lease
## apart from the job's earlier and later ones.
struct Lease
  id: Int
  payload: Text
  attempt: Int
  until: Time
end

## Why `lease` handed out no job.
enum LeaseError
  NothingReady                 # every job is leased, done or dead
  BadLength(length: Duration)  # not positive, or the lease would end after the year 9999
end

## Why `acknowledge` or `fail` refused a worker's report.
enum ReportError
  UnknownJob(id: Int)  # no job has this id
  NotHeld(id: Int)     # the lease ran out or was already reported: the job may be with another worker
end

## A queue with no jobs.
fn empty() : Queue
  Queue(jobs: {}, next_id: 1)
end

## Adds a job with this payload at the back of the queue, ready for its first
## try. Returns the new queue and the new job's id.
fn submit(queue: Queue, payload: Text) : (Queue, Int)
  promise "no job is lost"
    keeps_every_job?(queue, result.0)
    allows queue: queue_with([Job(id: 1, payload: "a", attempts: 0, state: Ready)]),
      payload: "b",
      result: (queue_with([Job(id: 1, payload: "a", attempts: 0, state: Ready),
        Job(id: 2, payload: "b", attempts: 0, state: Ready)]), 2)
    forbids queue: queue_with([Job(id: 1, payload: "a", attempts: 0, state: Ready)]),
      payload: "b",
      result: (queue_with([Job(id: 2, payload: "b", attempts: 0, state: Ready)]), 2)
  end
  promise "the new job gets a new id, and waits at the back, ready and untried"
    not queue.jobs.has?(result.1)
    result.0.jobs.keys == queue.jobs.keys.append(result.1)
    result.0.jobs.get(result.1) == Some(Job(id: result.1, payload: payload, attempts: 0, state: Ready))
    allows queue: empty(),
      payload: "a",
      result: (queue_with([Job(id: 1, payload: "a", attempts: 0, state: Ready)]), 1)
    forbids queue: empty(),
      payload: "a",
      result: (queue_with([Job(id: 1, payload: "a", attempts: 1, state: Ready)]), 1)
    forbids queue: queue_with([Job(id: 1, payload: "a", attempts: 0, state: Ready)]),
      payload: "b",
      result: (queue_with([Job(id: 2, payload: "b", attempts: 0, state: Ready),
        Job(id: 1, payload: "a", attempts: 0, state: Ready)]), 2)
  end

  id = queue.next_id
  job = Job(id: id, payload: payload, attempts: 0, state: Ready)
  (Queue(jobs: queue.jobs.set(id, job), next_id: id + 1), id)
end

## Leases the oldest ready job to a worker, from `now` for `length`, and counts
## the try. Jobs are oldest in the order they were submitted, and a job that
## failed keeps its place. Gives `NothingReady` when every job is leased, done
## or dead, and `BadLength` for a length that isn't positive or would end the
## lease after the year 9999. Leases that have run out are not taken back
## here: call `expire` first.
fn lease(queue: Queue, now: Time, length: Duration) : Result((Queue, Lease), LeaseError)
  promise "no job is lost"
    result is Ok((after, _)) implies keeps_every_job?(queue, after)
    allows queue: queue_with([Job(id: 1, payload: "a", attempts: 0, state: Ready)]),
      now: at(0),
      length: 30.seconds,
      result: Ok((queue_with([Job(id: 1, payload: "a", attempts: 1, state: Leased(until: at(30)))]),
        Lease(id: 1, payload: "a", attempt: 1, until: at(30))))
    forbids queue: queue_with([Job(id: 1, payload: "a", attempts: 0, state: Ready),
        Job(id: 2, payload: "b", attempts: 0, state: Ready)]),
      now: at(0),
      length: 30.seconds,
      result: Ok((queue_with([Job(id: 1, payload: "a", attempts: 1, state: Leased(until: at(30)))]),
        Lease(id: 1, payload: "a", attempt: 1, until: at(30))))
  end
  promise "the oldest ready job is leased from now for the length given, with its try counted, so no job is tried more than three times; nothing else changes"
    result is Ok((after, held)) implies changes_only?(queue, after, held.id)
    result is Ok((after, held)) implies after.jobs.get(held.id) ==
      Some(Job(id: held.id, payload: held.payload, attempts: held.attempt, state: Leased(until: held.until)))
    result is Ok((_, held)) implies oldest_ready(queue) is Some(oldest) and oldest.id == held.id and
      held.attempt == oldest.attempts + 1
    result is Ok((_, held)) implies held.attempt <= max_attempts and held.until > now and held.until == now + length
    result is Error(NothingReady) implies oldest_ready(queue) is None
    result is Error(BadLength(_)) implies length <= 0.seconds or now.checked_add(length) is None
    allows queue: queue_with([Job(id: 1, payload: "a", attempts: 1, state: Leased(until: at(30))),
        Job(id: 2, payload: "b", attempts: 2, state: Ready),
        Job(id: 3, payload: "c", attempts: 0, state: Ready)]),
      now: at(10),
      length: 30.seconds,
      result: Ok((queue_with([Job(id: 1, payload: "a", attempts: 1, state: Leased(until: at(30))),
          Job(id: 2, payload: "b", attempts: 3, state: Leased(until: at(40))),
          Job(id: 3, payload: "c", attempts: 0, state: Ready)]),
        Lease(id: 2, payload: "b", attempt: 3, until: at(40))))
    allows queue: queue_with([Job(id: 1, payload: "a", attempts: 1, state: Leased(until: at(30)))]),
      now: at(10),
      length: 30.seconds,
      result: Error(NothingReady)
    allows queue: empty(), now: at(10), length: 0.seconds, result: Error(BadLength(length: 0.seconds))
    forbids queue: queue_with([Job(id: 1, payload: "a", attempts: 0, state: Ready),
        Job(id: 2, payload: "b", attempts: 0, state: Ready)]),
      now: at(0),
      length: 30.seconds,
      result: Ok((queue_with([Job(id: 1, payload: "a", attempts: 0, state: Ready),
          Job(id: 2, payload: "b", attempts: 1, state: Leased(until: at(30)))]),
        Lease(id: 2, payload: "b", attempt: 1, until: at(30))))
    forbids queue: queue_with([Job(id: 1, payload: "a", attempts: 3, state: Ready)]),
      now: at(0),
      length: 30.seconds,
      result: Ok((queue_with([Job(id: 1, payload: "a", attempts: 4, state: Leased(until: at(30)))]),
        Lease(id: 1, payload: "a", attempt: 4, until: at(30))))
    forbids queue: queue_with([Job(id: 1, payload: "a", attempts: 0, state: Ready)]),
      now: at(0),
      length: 30.seconds,
      result: Error(NothingReady)
  end

  return Error(BadLength(length: length)) if length <= 0.seconds
  until = try now.checked_add(length).or_fail(BadLength(length: length))
  job = try oldest_ready(queue).or_fail(NothingReady)
  attempt = job.attempts + 1
  leased = Job(id: job.id, payload: job.payload, attempts: attempt, state: Leased(until: until))
  Ok((queue.put(leased), Lease(id: job.id, payload: job.payload, attempt: attempt, until: until)))
end

## Records that the worker holding this lease has done its job. The job stays
## in the queue, as done. Refused when the lease no longer holds the job; a
## report that comes after the lease's time, but before `expire` has taken the
## job back, still counts, since nobody else has the job.
fn acknowledge(queue: Queue, held: Lease) : Result(Queue, ReportError)
  promise "no job is lost"
    result is Ok(after) implies keeps_every_job?(queue, after)
    allows queue: queue_with([Job(id: 1, payload: "a", attempts: 1, state: Leased(until: at(30)))]),
      held: Lease(id: 1, payload: "a", attempt: 1, until: at(30)),
      result: Ok(queue_with([Job(id: 1, payload: "a", attempts: 1, state: Done)]))
    forbids queue: queue_with([Job(id: 1, payload: "a", attempts: 1, state: Leased(until: at(30)))]),
      held: Lease(id: 1, payload: "a", attempt: 1, until: at(30)),
      result: Ok(empty())
  end
  promise "only a lease that still holds its job is acknowledged, and then the job is done; nothing else changes"
    holds?(queue, held) == (result is Ok(_))
    result is Ok(after) implies changes_only?(queue, after, held.id)
    result is Ok(after) implies after.jobs.get(held.id) is Some(acknowledged) and
      acknowledged.state == Done and acknowledged.attempts == held.attempt
    queue.jobs.has?(held.id) or result == Error(UnknownJob(id: held.id))
    allows queue: queue_with([Job(id: 1, payload: "a", attempts: 1, state: Leased(until: at(30)))]),
      held: Lease(id: 1, payload: "a", attempt: 1, until: at(30)),
      result: Ok(queue_with([Job(id: 1, payload: "a", attempts: 1, state: Done)]))
    allows queue: queue_with([Job(id: 1, payload: "a", attempts: 2, state: Leased(until: at(90)))]),
      held: Lease(id: 1, payload: "a", attempt: 1, until: at(30)),
      result: Error(NotHeld(id: 1))
    allows queue: empty(),
      held: Lease(id: 1, payload: "a", attempt: 1, until: at(30)),
      result: Error(UnknownJob(id: 1))
    forbids queue: queue_with([Job(id: 1, payload: "a", attempts: 2, state: Leased(until: at(90)))]),
      held: Lease(id: 1, payload: "a", attempt: 1, until: at(30)),
      result: Ok(queue_with([Job(id: 1, payload: "a", attempts: 2, state: Done)]))
    forbids queue: queue_with([Job(id: 1, payload: "a", attempts: 1, state: Leased(until: at(30)))]),
      held: Lease(id: 1, payload: "a", attempt: 1, until: at(30)),
      result: Ok(queue_with([Job(id: 1, payload: "a", attempts: 1, state: Ready)]))
  end

  job = try held_job(queue, held)
  Ok(queue.put(job.with_state(Done)))
end

## Records that the worker holding this lease has failed at its job. The job
## is ready for another try, or dead if this was its third. Refused, as
## `acknowledge` is, when the lease no longer holds the job.
fn fail(queue: Queue, held: Lease) : Result(Queue, ReportError)
  promise "no job is lost"
    result is Ok(after) implies keeps_every_job?(queue, after)
    allows queue: queue_with([Job(id: 1, payload: "a", attempts: 3, state: Leased(until: at(30)))]),
      held: Lease(id: 1, payload: "a", attempt: 3, until: at(30)),
      result: Ok(queue_with([Job(id: 1, payload: "a", attempts: 3, state: Dead)]))
    forbids queue: queue_with([Job(id: 1, payload: "a", attempts: 3, state: Leased(until: at(30)))]),
      held: Lease(id: 1, payload: "a", attempt: 3, until: at(30)),
      result: Ok(empty())
  end
  promise "only a lease that still holds its job is failed, and then the job is ready for another try, or dead after its third; nothing else changes"
    holds?(queue, held) == (result is Ok(_))
    result is Ok(after) implies changes_only?(queue, after, held.id)
    result is Ok(after) implies queue.jobs.get(held.id) is Some(earlier) and
      after.jobs.get(held.id) is Some(later) and retried_or_dead?(earlier, later)
    queue.jobs.has?(held.id) or result == Error(UnknownJob(id: held.id))
    allows queue: queue_with([Job(id: 1, payload: "a", attempts: 1, state: Leased(until: at(30)))]),
      held: Lease(id: 1, payload: "a", attempt: 1, until: at(30)),
      result: Ok(queue_with([Job(id: 1, payload: "a", attempts: 1, state: Ready)]))
    allows queue: queue_with([Job(id: 1, payload: "a", attempts: 3, state: Leased(until: at(30)))]),
      held: Lease(id: 1, payload: "a", attempt: 3, until: at(30)),
      result: Ok(queue_with([Job(id: 1, payload: "a", attempts: 3, state: Dead)]))
    allows queue: queue_with([Job(id: 1, payload: "a", attempts: 1, state: Done)]),
      held: Lease(id: 1, payload: "a", attempt: 1, until: at(30)),
      result: Error(NotHeld(id: 1))
    forbids queue: queue_with([Job(id: 1, payload: "a", attempts: 3, state: Leased(until: at(30)))]),
      held: Lease(id: 1, payload: "a", attempt: 3, until: at(30)),
      result: Ok(queue_with([Job(id: 1, payload: "a", attempts: 3, state: Ready)]))
    forbids queue: queue_with([Job(id: 1, payload: "a", attempts: 1, state: Done)]),
      held: Lease(id: 1, payload: "a", attempt: 1, until: at(30)),
      result: Ok(queue_with([Job(id: 1, payload: "a", attempts: 1, state: Ready)]))
  end

  job = try held_job(queue, held)
  Ok(queue.put(job.after_try))
end

## Takes back every job whose lease has run out by `now`, as a failed try: the
## job is ready for another try, or dead if that was its third.
fn expire(queue: Queue, now: Time) : Queue
  promise "no job is lost"
    keeps_every_job?(queue, result)
    allows queue: queue_with([Job(id: 1, payload: "a", attempts: 3, state: Leased(until: at(30)))]),
      now: at(30),
      result: queue_with([Job(id: 1, payload: "a", attempts: 3, state: Dead)])
    forbids queue: queue_with([Job(id: 1, payload: "a", attempts: 3, state: Leased(until: at(30)))]),
      now: at(30),
      result: empty()
  end
  promise "every lease that has run out ends as a failed try, so its job is ready for another, or dead after its third; nothing else changes"
    result.jobs.keys == queue.jobs.keys and result.next_id == queue.next_id
    result.jobs.values.all?(fn(job) not job.expired?(now) end)
    queue.jobs.values.all?(fn(job) not job.expired?(now) implies result.jobs.get(job.id) == Some(job) end)
    queue.jobs.values.all?(fn(job)
      job.expired?(now) implies result.jobs.get(job.id) is Some(later) and retried_or_dead?(job, later)
    end)
    allows queue: queue_with([Job(id: 1, payload: "a", attempts: 1, state: Leased(until: at(30))),
        Job(id: 2, payload: "b", attempts: 3, state: Leased(until: at(30))),
        Job(id: 3, payload: "c", attempts: 1, state: Leased(until: at(60)))]),
      now: at(30),
      result: queue_with([Job(id: 1, payload: "a", attempts: 1, state: Ready),
        Job(id: 2, payload: "b", attempts: 3, state: Dead),
        Job(id: 3, payload: "c", attempts: 1, state: Leased(until: at(60)))])
    forbids queue: queue_with([Job(id: 1, payload: "a", attempts: 1, state: Leased(until: at(30)))]),
      now: at(30),
      result: queue_with([Job(id: 1, payload: "a", attempts: 1, state: Leased(until: at(30)))])
    forbids queue: queue_with([Job(id: 1, payload: "a", attempts: 1, state: Leased(until: at(60)))]),
      now: at(30),
      result: queue_with([Job(id: 1, payload: "a", attempts: 1, state: Ready)])
  end

  var next = queue
  for candidate in queue.jobs.values
    if candidate.expired?(now)
      next = next.put(candidate.after_try)
    end
  end
  next
end

## Takes the finished jobs, done and dead, out of the queue and hands them
## back, oldest first, so that a queue doesn't grow without end. Their ids are
## not given out again.
fn take_finished(queue: Queue) : (Queue, List(Job))
  promise "no job is lost: each one is still in the queue, or handed back"
    queue.jobs.values.all?(fn(job) result.0.jobs.get(job.id) == Some(job) or result.1.contains?(job) end)
    allows queue: queue_with([Job(id: 1, payload: "a", attempts: 1, state: Done),
        Job(id: 2, payload: "b", attempts: 0, state: Ready)]),
      result: (queue_with([Job(id: 2, payload: "b", attempts: 0, state: Ready)]),
        [Job(id: 1, payload: "a", attempts: 1, state: Done)])
    forbids queue: queue_with([Job(id: 1, payload: "a", attempts: 1, state: Done),
        Job(id: 2, payload: "b", attempts: 0, state: Ready)]),
      result: (queue_with([Job(id: 2, payload: "b", attempts: 0, state: Ready)]), [])
  end
  promise "only done and dead jobs are taken out; the others stay, in their order, and no id is given out twice"
    result.1.all?(fn(job) finished?(job) end)
    result.0.jobs.values == queue.jobs.values.filter(fn(job) not finished?(job) end)
    result.0.next_id == queue.next_id
    allows queue: queue_with([Job(id: 1, payload: "a", attempts: 1, state: Done),
        Job(id: 2, payload: "b", attempts: 0, state: Ready)]),
      result: (queue_with([Job(id: 2, payload: "b", attempts: 0, state: Ready)]),
        [Job(id: 1, payload: "a", attempts: 1, state: Done)])
    forbids queue: queue_with([Job(id: 1, payload: "a", attempts: 1, state: Done),
        Job(id: 2, payload: "b", attempts: 0, state: Ready)]),
      result: (queue_with([Job(id: 1, payload: "a", attempts: 1, state: Done),
        Job(id: 2, payload: "b", attempts: 0, state: Ready)]), [])
  end

  taken = queue.jobs.values.filter(finished?)
  var rest = queue
  for finished_job in taken
    rest.jobs = rest.jobs.remove(finished_job.id)
  end
  (rest, taken)
end

## The job with this id, in whatever state it is.
fn find_job(queue: Queue, id: Int) : Option(Job)
  queue.jobs.get(id)
end

## Every job in the queue, oldest first.
fn jobs(queue: Queue) : List(Job)
  queue.jobs.values
end

# The workings.

## The oldest job that is ready to be leased.
fn oldest_ready(queue: Queue) : Option(Job)
  examples
    oldest_ready(empty()) == None
    oldest_ready(queue_with([Job(id: 1, payload: "a", attempts: 1, state: Leased(until: at(30))),
      Job(id: 2, payload: "b", attempts: 1, state: Ready),
      Job(id: 3, payload: "c", attempts: 0, state: Ready)])) ==
      Some(Job(id: 2, payload: "b", attempts: 1, state: Ready))
  end

  queue.jobs.values.find(fn(job) job.state == Ready end)
end

## The queue with this job stored under its id, in the place it had.
fn put(queue: Queue, job: Job) : Queue
  var next = queue
  next.jobs = next.jobs.set(job.id, job)
  next
end

## The job, moved to another state.
fn with_state(job: Job, state: State) : Job
  var changed = job
  changed.state = state
  changed
end

## A job after a failed try: ready for another if it has one left, and dead if
## not.
fn after_try(job: Job) : Job
  examples
    after_try(Job(id: 1, payload: "a", attempts: 1, state: Leased(until: at(30)))) ==
      Job(id: 1, payload: "a", attempts: 1, state: Ready)
    after_try(Job(id: 1, payload: "a", attempts: 2, state: Leased(until: at(30)))) ==
      Job(id: 1, payload: "a", attempts: 2, state: Ready)
    after_try(Job(id: 1, payload: "a", attempts: 3, state: Leased(until: at(30)))) ==
      Job(id: 1, payload: "a", attempts: 3, state: Dead)
  end

  next_state = if job.attempts < max_attempts: Ready else: Dead
  job.with_state(next_state)
end

## Whether the job is leased, and its lease has run out by `now`.
fn expired?(job: Job, now: Time) : Bool
  examples
    expired?(Job(id: 1, payload: "a", attempts: 1, state: Leased(until: at(30))), at(30))
    not expired?(Job(id: 1, payload: "a", attempts: 1, state: Leased(until: at(30))), at(29))
    not expired?(Job(id: 1, payload: "a", attempts: 0, state: Ready), at(30))
  end

  job.state is Leased(until: deadline) and deadline <= now
end

## Whether the queue still has this lease's job out on this lease: not taken
## back by `expire`, not reported yet, and so not leased again since.
fn holds?(queue: Queue, held: Lease) : Bool
  queue.jobs.get(held.id) is Some(job) and job.state is Leased(until: _) and job.attempts == held.attempt
end

## The job this lease is for, if the lease still holds it.
fn held_job(queue: Queue, held: Lease) : Result(Job, ReportError)
  job = try queue.jobs.get(held.id).or_fail(UnknownJob(id: held.id))
  return Error(NotHeld(id: held.id)) if not holds?(queue, held)
  Ok(job)
end

# What the promises say.

## Whether `after` still holds every job that `before` held, in the same order.
## This is what "no job is lost" means: a job may move on, from ready to leased
## and from leased to done, ready or dead, but it is never dropped or replaced,
## and a done or dead job never comes back.
fn keeps_every_job?(before: Queue, after: Queue) : Bool
  examples
    keeps_every_job?(empty(), empty())
    keeps_every_job?(queue_with([Job(id: 1, payload: "a", attempts: 1, state: Leased(until: at(30)))]),
      queue_with([Job(id: 1, payload: "a", attempts: 1, state: Ready)]))
    keeps_every_job?(queue_with([Job(id: 1, payload: "a", attempts: 0, state: Ready)]),
      queue_with([Job(id: 1, payload: "a", attempts: 0, state: Ready),
        Job(id: 2, payload: "b", attempts: 0, state: Ready)]))
    not keeps_every_job?(queue_with([Job(id: 1, payload: "a", attempts: 0, state: Ready)]), empty())
    not keeps_every_job?(queue_with([Job(id: 1, payload: "a", attempts: 0, state: Ready)]),
      queue_with([Job(id: 1, payload: "b", attempts: 0, state: Ready)]))
    not keeps_every_job?(queue_with([Job(id: 1, payload: "a", attempts: 1, state: Done)]),
      queue_with([Job(id: 1, payload: "a", attempts: 1, state: Ready)]))
    not keeps_every_job?(queue_with([Job(id: 1, payload: "a", attempts: 0, state: Ready),
        Job(id: 2, payload: "b", attempts: 0, state: Ready)]),
      queue_with([Job(id: 2, payload: "b", attempts: 0, state: Ready),
        Job(id: 1, payload: "a", attempts: 0, state: Ready)]))
  end

  after.jobs.keys.take(before.jobs.size) == before.jobs.keys and
    before.jobs.values.all?(fn(job) after.still_holds?(job) end)
end

## Whether the queue still holds this job: under its id, with its payload,
## tried no fewer times, and in the same state if it was done or dead.
fn still_holds?(queue: Queue, job: Job) : Bool
  queue.jobs.get(job.id) is Some(kept) and
    kept.payload == job.payload and
    kept.attempts >= job.attempts and
    (finished?(job) implies kept.state == job.state)
end

## Whether no worker will run the job again.
fn finished?(job: Job) : Bool
  job.state == Done or job.state == Dead
end

## Whether `after` holds the same jobs as `before`, in the same order, with
## every job but the one with this id unchanged.
fn changes_only?(before: Queue, after: Queue, id: Int) : Bool
  after.jobs.keys == before.jobs.keys and
    after.jobs.remove(id) == before.jobs.remove(id) and
    after.next_id == before.next_id
end

## Whether `after` is `before` after a failed try: the same job, with the same
## count of tries, ready for another try if it has one left, and dead if not.
fn retried_or_dead?(before: Job, after: Job) : Bool
  after.id == before.id and after.payload == before.payload and
    after.attempts == before.attempts and
    after.state == (if before.attempts < max_attempts: Ready else: Dead)
end

# Samples for examples, promises and tests.

## A time `offset` seconds after nine o'clock on 12 October 2026, UTC.
fn at(offset: Int) : Time
  nine = Time.parse("2026-10-12T09:00:00Z") or stop "this is a valid RFC 3339 time"
  nine + offset.seconds
end

## A queue holding these jobs, in this order, oldest first.
fn queue_with(contents: List(Job)) : Queue
  ids = contents.map(fn(job) job.id end)
  Queue(jobs: ids.zip(contents).to_map, next_id: (ids.max or 0) + 1)
end

test "jobs are leased oldest first, and an acknowledged job stays, as done"
  (one, first_id) = empty().submit("resize photo 1")
  (two, second_id) = one.submit("resize photo 2")
  assert two.lease(at(0), 30.seconds) is Ok((first_out, first_lease))
  assert first_lease == Lease(id: first_id, payload: "resize photo 1", attempt: 1, until: at(30))
  assert two.find_job(first_id) == Some(Job(id: first_id, payload: "resize photo 1", attempts: 0, state: Ready))
  assert first_out.lease(at(5), 30.seconds) is Ok((both_out, second_lease))
  assert second_lease.id == second_id
  assert both_out.lease(at(10), 30.seconds) == Error(NothingReady)
  assert both_out.acknowledge(first_lease) is Ok(first_done)
  assert first_done.find_job(first_id) ==
    Some(Job(id: first_id, payload: "resize photo 1", attempts: 1, state: Done))
  assert first_done.acknowledge(first_lease) == Error(NotHeld(id: first_id))
  assert first_done.jobs.size == 2
end

test "a job is tried at most three times, then it is dead"
  (submitted, id) = empty().submit("charge card")
  assert submitted.lease(at(0), 10.seconds) is Ok((tried_once, lease_one))
  assert tried_once.fail(lease_one) is Ok(failed_once)
  assert failed_once.lease(at(1), 10.seconds) is Ok((tried_twice, lease_two))
  assert tried_twice.fail(lease_two) is Ok(failed_twice)
  assert failed_twice.lease(at(2), 10.seconds) is Ok((tried_thrice, lease_three))
  assert lease_three.attempt == 3
  assert tried_thrice.fail(lease_three) is Ok(failed_thrice)
  assert failed_thrice.find_job(id) == Some(Job(id: id, payload: "charge card", attempts: 3, state: Dead))
  assert failed_thrice.lease(at(3), 10.seconds) == Error(NothingReady)
end

test "a lease that runs out is taken back, counting the try, and its holder can't report any more"
  (submitted, id) = empty().submit("send report")
  assert submitted.lease(at(0), 30.seconds) is Ok((leased, stale))
  assert leased.expire(at(29)) == leased
  taken_back = leased.expire(at(30))
  assert taken_back.find_job(id) == Some(Job(id: id, payload: "send report", attempts: 1, state: Ready))
  assert taken_back.acknowledge(stale) == Error(NotHeld(id: id))
  assert taken_back.lease(at(31), 30.seconds) is Ok((leased_again, fresh))
  assert fresh.attempt == 2
  assert leased_again.fail(stale) == Error(NotHeld(id: id))
  assert leased_again.acknowledge(fresh) is Ok(_)
end

test "a job whose third lease runs out is dead"
  (submitted, id) = empty().submit("sync inbox")
  assert submitted.lease(at(0), 10.seconds) is Ok((first_out, _))
  assert first_out.expire(at(10)).lease(at(10), 10.seconds) is Ok((second_out, _))
  assert second_out.expire(at(20)).lease(at(20), 10.seconds) is Ok((third_out, _))
  dead = third_out.expire(at(30))
  assert dead.find_job(id) == Some(Job(id: id, payload: "sync inbox", attempts: 3, state: Dead))
  assert dead.lease(at(30), 10.seconds) == Error(NothingReady)
end

test "finished jobs are taken out and handed back, and their ids are not given out again"
  (one, done_id) = empty().submit("a")
  (two, waiting_id) = one.submit("b")
  assert two.lease(at(0), 30.seconds) is Ok((leased, held))
  assert leased.acknowledge(held) is Ok(acknowledged)
  (rest, taken) = acknowledged.take_finished
  assert taken == [Job(id: done_id, payload: "a", attempts: 1, state: Done)]
  assert rest.jobs.values == [Job(id: waiting_id, payload: "b", attempts: 0, state: Ready)]
  (three, new_id) = rest.submit("c")
  assert new_id != done_id and new_id != waiting_id
  assert three.jobs.size == 2
end

test "a lease needs a length that ends in time, and a report needs a known job"
  (submitted, _) = empty().submit("a")
  assert submitted.lease(at(0), 0.seconds) == Error(BadLength(length: 0.seconds))
  assert submitted.lease(at(0), (-5).seconds) == Error(BadLength(length: (-5).seconds))
  last_second = Time.parse("9999-12-31T23:59:59Z") or stop "this is a valid RFC 3339 time"
  assert submitted.lease(last_second, 1.days) == Error(BadLength(length: 1.days))
  forged = Lease(id: 99, payload: "a", attempt: 1, until: at(30))
  assert submitted.acknowledge(forged) == Error(UnknownJob(id: 99))
  assert submitted.fail(forged) == Error(UnknownJob(id: 99))
end

test "a long run of random operations loses no job"
  random = Random.seeded(2026)
  var queue = empty()
  var handed_out = []
  var submitted = 0
  for step in 0..1000
    now = at(step)
    before = queue
    case random.int(0, 4)
      0:
        (with_new, _) = queue.submit("job #{step}")
        queue = with_new
        submitted += 1
      1:
        if queue.lease(now, random.int(1, 30).seconds) is Ok((leased, given))
          queue = leased
          handed_out = handed_out.append(given)
        end
      2: queue = queue.expire(now)
      _:
        if handed_out.get(random.int(0, handed_out.size + 1)) is Some(given)
          reported = if random.int(0, 2) == 0: queue.acknowledge(given) else: queue.fail(given)
          if reported is Ok(after)
            queue = after
          end
        end
    end
    assert keeps_every_job?(before, queue)
  end
  assert queue.jobs.size == submitted
  assert queue.jobs.values.all?(fn(job) job.attempts <= max_attempts end)
end
