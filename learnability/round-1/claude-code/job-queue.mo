## The core of a job queue, as a value.
##
## Jobs are submitted, then leased out oldest first: a job that is tried again
## keeps its place ahead of newer jobs. A lease lasts a given length of time,
## and each lease is one try of its job. The worker holding a lease
## acknowledges the job when its work is done, or fails it; `expire` takes
## back the leases that have run out. A failed or expired job is ready to try
## again, unless that was its third try: then it is dead.
##
## Time comes in only as arguments: `lease` and `expire` are told the time, so
## every operation computes a new queue from the old one and touches nothing
## else.
##
## No operation loses a job, and each one promises so. Acknowledged and dead
## jobs stay in the queue too, so every job ever submitted can be looked up.
module JobQueue
expose JobQueue, JobId, Job(..), JobState(..), Lease, LeaseError(..),
  SettleError(..), max_tries, empty, submit, lease, acknowledge, fail, expire,
  find, jobs, job_id, payload, attempt, until

## A job is tried at most this many times: when its last try fails or runs
## out, it is dead.
max_tries = 3

## Names a job in its queue. Jobs are numbered from 1, in the order they were
## submitted.
struct JobId
  number: Int
end

## Where a job stands.
enum JobState
  Ready                 # waiting for a worker
  Leased(until: Time)   # held by a worker, until the given time
  Done                  # acknowledged: its work is finished
  Dead                  # failed or ran out on every one of its tries
end

## A job: its id, its work, how many times it has been leased, and where it
## stands.
struct Job
  id: JobId
  payload: Text
  tries: Int
  state: JobState
end

## A job queue: every job ever submitted, by id, oldest first. Its insides are
## hidden, so every queue is one that this module's operations built.
struct JobQueue
  by_id: Map(JobId, Job)
end

## A job handed to a worker: which job, its work, which try of it this is, and
## when the lease runs out. Its insides are hidden: only `lease` makes one, and
## `job_id`, `payload`, `attempt` and `until` read it.
struct Lease
  job_id: JobId
  payload: Text
  attempt: Int
  until: Time
end

## Why `lease` handed out no job.
enum LeaseError
  NothingReady          # no job is waiting
  BadLength(Duration)   # a lease must last longer than zero, and end by the year 9999
end

## Why `acknowledge` or `fail` refused a lease.
enum SettleError
  UnknownJob(JobId)     # the queue has no job with the lease's id
  NotHeld(JobId)        # the lease no longer holds its job: it was settled, or taken back
end

## A queue with no jobs.
fn empty() : JobQueue
  JobQueue(by_id: {})
end

## Adds a job with the given work at the back of the queue, ready, and gives
## its id.
fn submit(queue: JobQueue, payload: Text) : (JobQueue, JobId)
  promise "no job is lost: every queued job is kept as it was, and the new job joins them at the back, ready"
    result.0.jobs == queue.jobs.append(Job(id: result.1, payload: payload, tries: 0, state: Ready))
    allows queue: empty(), payload: "a", result: (single(0, Ready), JobId(number: 1))
    forbids queue: single(0, Ready), payload: "b", result: (single(0, Ready), JobId(number: 2))
  end
  examples
    submit(empty(), "a") == (single(0, Ready), JobId(number: 1))
    submit(single(0, Ready), "b").1 == JobId(number: 2)
  end

  id = JobId(number: queue.by_id.size + 1)
  job = Job(id: id, payload: payload, tries: 0, state: Ready)
  (JobQueue(by_id: queue.by_id.set(id, job)), id)
end

## Leases the oldest ready job, from now for the given length of time. The
## lease is the job's next try.
fn lease(queue: JobQueue, now: Time, length: Duration) : Result((JobQueue, Lease), LeaseError)
  promise "no job is lost: the oldest ready job, if there is one, is leased from now for the given time as its next try, never past the last, and every other job is kept as it was"
    result is Ok((after, given)) implies
      work(after) == work(queue) and
      others(after, given.job_id) == others(queue, given.job_id) and
      queue.jobs.find(ready?) ==
        Some(Job(id: given.job_id, payload: given.payload, tries: given.attempt - 1, state: Ready)) and
      after.find(given.job_id) == Some(leased_job(given)) and
      now.checked_add(length) == Some(given.until) and
      given.attempt <= max_tries
    result is Error(NothingReady) implies not queue.jobs.any?(ready?)
    allows queue: single(0, Ready), now: noon, length: 1.minutes,
      result: Ok((single(1, Leased(until: noon + 1.minutes)), lease_one(1, noon + 1.minutes)))
    allows queue: single(1, Leased(until: noon)), now: noon, length: 1.minutes,
      result: Error(NothingReady)
    forbids queue: single(0, Ready), now: noon, length: 1.minutes,
      result: Ok((empty(), lease_one(1, noon + 1.minutes)))
    forbids queue: single(0, Ready), now: noon, length: 1.minutes, result: Error(NothingReady)
  end
  examples
    lease(empty(), noon, 1.minutes) == Error(NothingReady)
    lease(single(0, Ready), noon, 0.seconds) == Error(BadLength(0.seconds))
  end

  return Error(BadLength(length)) if length <= 0.seconds
  deadline = try now.checked_add(length).or_fail(BadLength(length))
  job = try queue.jobs.find(ready?).or_fail(NothingReady)
  held = Lease(job_id: job.id, payload: job.payload, attempt: job.tries + 1, until: deadline)
  Ok((put(queue, leased_job(held)), held))
end

## Marks the job a lease holds as done. A lease holds its job until it is
## settled or `expire` takes it back, so a worker that finishes a little late
## can still acknowledge its job.
fn acknowledge(queue: JobQueue, held: Lease) : Result(JobQueue, SettleError)
  promise "no job is lost: only the job a lease holds is acknowledged, and it is kept, done; every other job is kept as it was"
    result is Ok(after) implies
      queue.find(held.job_id) == Some(leased_job(held)) and
      work(after) == work(queue) and
      others(after, held.job_id) == others(queue, held.job_id) and
      state_of(after, held.job_id) == Some(Done)
    allows queue: single(1, Leased(until: noon)), held: lease_one(1, noon), result: Ok(single(1, Done))
    forbids queue: single(1, Leased(until: noon)), held: lease_one(1, noon), result: Ok(empty())
    forbids queue: single(0, Ready), held: lease_one(1, noon), result: Ok(single(0, Done))
  end

  var job = try held_job(queue, held)
  job.state = Done
  Ok(put(queue, job))
end

## Takes back the job a lease holds after a failed try: it is ready to try
## again, or dead if that was its last try.
fn fail(queue: JobQueue, held: Lease) : Result(JobQueue, SettleError)
  promise "no job is lost: only the job a lease holds fails, and it is kept, ready for another try or dead after its last; every other job is kept as it was"
    result is Ok(after) implies
      queue.find(held.job_id) == Some(leased_job(held)) and
      work(after) == work(queue) and
      others(after, held.job_id) == others(queue, held.job_id) and
      state_of(after, held.job_id) == Some(if held.attempt < max_tries: Ready else: Dead)
    allows queue: single(1, Leased(until: noon)), held: lease_one(1, noon), result: Ok(single(1, Ready))
    allows queue: single(3, Leased(until: noon)), held: lease_one(3, noon), result: Ok(single(3, Dead))
    forbids queue: single(1, Leased(until: noon)), held: lease_one(1, noon), result: Ok(empty())
    forbids queue: single(3, Leased(until: noon)), held: lease_one(3, noon), result: Ok(single(3, Ready))
  end

  job = try held_job(queue, held)
  Ok(put(queue, given_back(job)))
end

## Takes back every job whose lease has run out by now, as if its try had
## failed.
fn expire(queue: JobQueue, now: Time) : JobQueue
  promise "no job is lost: each job whose lease ran out is kept, ready for another try or dead after its last, and every other job is kept as it was"
    work(result) == work(queue)
    queue.jobs.zip(result.jobs).all?(fn(pair)
      (before, after) = pair
      if ran_out?(before, now)
        after.tries == before.tries and after.state == (if before.tries < max_tries: Ready else: Dead)
      else
        after == before
      end
    end)
    allows queue: single(1, Leased(until: noon)), now: noon, result: single(1, Ready)
    allows queue: single(3, Leased(until: noon)), now: noon, result: single(3, Dead)
    allows queue: single(1, Leased(until: noon + 1.minutes)), now: noon,
      result: single(1, Leased(until: noon + 1.minutes))
    forbids queue: single(1, Leased(until: noon)), now: noon, result: empty()
    forbids queue: single(1, Leased(until: noon + 1.minutes)), now: noon, result: single(1, Ready)
  end

  var next = queue
  for job in queue.jobs
    if ran_out?(job, now)
      next = put(next, given_back(job))
    end
  end
  next
end

## The job with the given id, if the queue has it.
fn find(queue: JobQueue, id: JobId) : Option(Job)
  queue.by_id.get(id)
end

## Every job in the queue, oldest first, whatever its state.
fn jobs(queue: JobQueue) : List(Job)
  queue.by_id.values
end

## The id of the job a lease holds.
fn job_id(held: Lease) : JobId
  held.job_id
end

## The work of the job a lease holds.
fn payload(held: Lease) : Text
  held.payload
end

## Which try of its job a lease is: 1 for the first, up to `max_tries`.
fn attempt(held: Lease) : Int
  held.attempt
end

## When a lease runs out.
fn until(held: Lease) : Time
  held.until
end

## Whether a job is waiting for a worker.
fn ready?(job: Job) : Bool
  job.state == Ready
end

## Whether a job is leased, and its lease has run out by now.
fn ran_out?(job: Job, now: Time) : Bool
  job.state is Leased(until: deadline) and deadline <= now
end

## A job as a lease holds it: leased until the lease runs out, and tried as
## many times as the lease's try says.
fn leased_job(held: Lease) : Job
  Job(id: held.job_id, payload: held.payload, tries: held.attempt, state: Leased(until: held.until))
end

## The job a lease holds, while the lease lasts: until the job is acknowledged,
## failed, or taken back by `expire`.
fn held_job(queue: JobQueue, held: Lease) : Result(Job, SettleError)
  job = try queue.by_id.get(held.job_id).or_fail(UnknownJob(held.job_id))
  if job == leased_job(held): Ok(job) else: Error(NotHeld(held.job_id))
end

## A job taken back after a failed try: ready again if it has tries left,
## otherwise dead.
fn given_back(job: Job) : Job
  var back = job
  back.state = if job.tries < max_tries: Ready else: Dead
  back
end

## The queue with the given job stored under its id, in its place.
fn put(queue: JobQueue, job: Job) : JobQueue
  JobQueue(by_id: queue.by_id.set(job.id, job))
end

## Each job's id and work, oldest first: what no operation may lose.
fn work(queue: JobQueue) : List((JobId, Text))
  queue.jobs.map(fn(job) (job.id, job.payload) end)
end

## Every job but the one with the given id, oldest first.
fn others(queue: JobQueue, id: JobId) : List(Job)
  queue.jobs.filter(fn(job) job.id != id end)
end

## Where the job with the given id stands, if the queue has it.
fn state_of(queue: JobQueue, id: JobId) : Option(JobState)
  queue.find(id).map(fn(job) job.state end)
end

# Values for the anchors of promises, and for the tests.

## Noon on 12 October 2026, UTC.
noon = case Time.parse("2026-10-12T12:00:00Z")
  Some(time): time
  None: stop "the text is a valid RFC 3339 time"
end

## A queue holding only job 1, whose work is "a", tried and standing as given.
fn single(tries: Int, state: JobState) : JobQueue
  id = JobId(number: 1)
  JobQueue(by_id: {id: Job(id: id, payload: "a", tries: tries, state: state)})
end

## A lease of the job in `single`, for the given try, running out at the given
## time.
fn lease_one(tries: Int, ends: Time) : Lease
  Lease(job_id: JobId(number: 1), payload: "a", attempt: tries, until: ends)
end

test "jobs are leased oldest first, and a leased job is not leased again"
  (one, first) = submit(empty(), "first")
  (two, second) = submit(one, "second")
  assert lease(two, noon, 1.minutes) is Ok((three, held_first))
  assert held_first.job_id == first
  assert held_first.payload == "first"
  assert held_first.attempt == 1
  assert held_first.until == noon + 1.minutes
  assert lease(three, noon, 1.minutes) is Ok((four, held_second))
  assert held_second.job_id == second
  assert lease(four, noon, 1.minutes) == Error(NothingReady)
end

test "a failed job is tried again before newer jobs"
  (one, old) = submit(empty(), "old")
  (two, _) = submit(one, "new")
  assert lease(two, noon, 1.minutes) is Ok((leased, held))
  assert fail(leased, held) is Ok(failed)
  assert lease(failed, noon, 1.minutes) is Ok((_, again))
  assert again.job_id == old
  assert again.attempt == 2
end

test "a job that fails on each of its three tries is dead, and is not leased again"
  (start, id) = submit(empty(), "flaky")
  var queue = start
  for round in 1..max_tries + 1
    assert lease(queue, noon, 1.minutes) is Ok((leased, held))
    assert held.attempt == round
    assert fail(leased, held) is Ok(failed)
    queue = failed
  end
  assert state_of(queue, id) == Some(Dead)
  assert lease(queue, noon, 1.minutes) == Error(NothingReady)
end

test "a lease that runs out is taken back as a try, and its holder can no longer settle it"
  (start, id) = submit(empty(), "slow")
  assert lease(start, noon, 1.minutes) is Ok((leased, late))
  assert expire(leased, noon + 59.seconds) == leased
  taken_back = expire(leased, noon + 1.minutes)
  assert state_of(taken_back, id) == Some(Ready)
  assert acknowledge(taken_back, late) == Error(NotHeld(id))
  assert lease(taken_back, noon + 2.minutes, 1.minutes) is Ok((again, retry))
  assert retry.attempt == 2
  assert fail(again, late) == Error(NotHeld(id))
  assert acknowledge(again, retry) is Ok(finished)
  assert state_of(finished, id) == Some(Done)
  assert acknowledge(finished, retry) == Error(NotHeld(id))
end

test "a job whose last lease runs out is dead"
  (start, id) = submit(empty(), "stuck")
  var queue = start
  for _ in 0..max_tries
    assert lease(queue, noon, 1.minutes) is Ok((leased, _))
    queue = expire(leased, noon + 1.minutes)
  end
  assert state_of(queue, id) == Some(Dead)
end

test "a lease must last some time, and end by the year 9999"
  (queue, _) = submit(empty(), "a")
  assert lease(queue, noon, 0.seconds) == Error(BadLength(0.seconds))
  assert lease(queue, noon, 3_000_000.days) == Error(BadLength(3_000_000.days))
end

test "a lease from another queue is refused"
  (mine, id) = submit(empty(), "mine")
  (theirs, _) = submit(empty(), "theirs")
  assert lease(mine, noon, 1.minutes) is Ok((_, held))
  assert lease(theirs, noon, 1.minutes) is Ok((leased_theirs, _))
  assert acknowledge(empty(), held) == Error(UnknownJob(id))
  assert fail(empty(), held) == Error(UnknownJob(id))
  assert acknowledge(leased_theirs, held) == Error(NotHeld(id))
  assert fail(leased_theirs, held) == Error(NotHeld(id))
end

test "done and dead jobs stay in the queue, beside the ready ones"
  (one, done_id) = submit(empty(), "a")
  (two, dead_id) = submit(one, "b")
  (three, ready_id) = submit(two, "c")
  assert lease(three, noon, 1.minutes) is Ok((leased, held))
  assert acknowledge(leased, held) is Ok(acknowledged)
  var queue = acknowledged
  for _ in 0..max_tries
    assert lease(queue, noon, 1.minutes) is Ok((retried, held_again))
    assert held_again.job_id == dead_id
    assert fail(retried, held_again) is Ok(failed)
    queue = failed
  end
  assert queue.jobs.map(fn(job) job.state end) == [Done, Dead, Ready]
  assert work(queue) == [(done_id, "a"), (dead_id, "b"), (ready_id, "c")]
end
