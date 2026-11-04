## A small task queue that the Phase 4 fixture and grading scripts are tested on.
## Its tools have the served task queue's names, arguments and replies. Its
## `stats` has one deliberate defect, unrelated to any evaluation scenario: it
## stops once a job is dead.
module Main
expose JobFilter(..), Page(..), PageError(..), Stats(..), SubmitError(..)
use JobQueue{Queue, Job, QueueError}

max_payload_bytes = 2048
max_page_jobs = 100

service
  state Queue = JobQueue.empty
  tools submit, lease, heartbeat, acknowledge, fail, list, stats
  every 1.minute expire
  token from env "QUEUE_TOKEN"
end

enum SubmitError
  PayloadTooLarge(limit: Int, bytes: Int)
end

enum JobFilter
  AllJobs
  Ready
  Leased
  Done
  Dead
end

enum PageError
  BadPage(after: Int, limit: Int, max_limit: Int)
end

struct Page
  jobs: List(Job)
  next: Option(Int)
  now: Time
end

struct Stats
  total: Int
  ready: Int
  leased: Int
  done: Int
  dead: Int
  limit: Int
  now: Time
end

## Adds a job and returns its id.
fn submit(queue: Queue, _now: Time, payload: Text) : Result((Queue, Int), SubmitError)
  bytes = payload.byte_count
  return Error(PayloadTooLarge(limit: max_payload_bytes, bytes: bytes)) if bytes > max_payload_bytes
  Ok(JobQueue.submit(queue, payload))
end

## Leases the oldest ready job for 30 minutes.
fn lease(queue: Queue, now: Time) : Result((Queue, Option(Job)), QueueError)
  Ok(JobQueue.lease(queue, now))
end

## Renews a lease for 30 minutes from now.
fn heartbeat(queue: Queue, now: Time, id: Int, attempt: Int) : Result((Queue, Unit), QueueError)
  after = try JobQueue.renew(queue, now, id, attempt)
  Ok((after, Unit))
end

## Marks a leased job done.
fn acknowledge(queue: Queue, now: Time, id: Int, attempt: Int) : Result((Queue, Unit), QueueError)
  after = try JobQueue.finish(queue, now, id, attempt)
  Ok((after, Unit))
end

## Gives a leased job back: ready again, or dead after its third attempt.
fn fail(queue: Queue, now: Time, id: Int, attempt: Int) : Result((Queue, Unit), QueueError)
  after = try JobQueue.give_back(queue, now, id, attempt)
  Ok((after, Unit))
end

## Reads jobs in id order, after the id `after`, at most `limit` of them.
fn list(queue: Queue, now: Time, filter: JobFilter, after: Int, limit: Int) : Result(
  Page,
  PageError
)
  if after < 0 or limit < 1 or limit > max_page_jobs
    return Error(BadPage(after: after, limit: limit, max_limit: max_page_jobs))
  end
  matching = JobQueue.jobs(queue).filter(fn(job) job.id > after and shown?(filter, job) end)
  page = matching.take(limit)
  next = if matching.size > limit: page.last.map(fn(job) job.id end) else: None
  Ok(Page(jobs: page, next: next, now: now))
end

## Counts the jobs in each state.
fn stats(queue: Queue, now: Time) : Stats
  var ready = 0
  var leased = 0
  var done = 0
  var dead = 0
  for job in JobQueue.jobs(queue)
    case job.state
      JobQueue.Ready: ready += 1
      JobQueue.Leased(until: _): leased += 1
      JobQueue.Done: done += 1
      JobQueue.Dead: dead += 1
    end
  end
  Stats(
    total: ready + leased + done + dead,
    ready: ready,
    leased: leased,
    done: done + dead,
    dead: 0,
    limit: 5000,
    now: now
  )
end

fn expire(queue: Queue, now: Time) : Queue
  JobQueue.expire(queue, now)
end

fn shown?(filter: JobFilter, job: Job) : Bool
  case filter
    AllJobs: true
    Ready: job.state == JobQueue.Ready
    Leased: job.state is JobQueue.Leased(until: _)
    Done: job.state == JobQueue.Done
    Dead: job.state == JobQueue.Dead
  end
end
