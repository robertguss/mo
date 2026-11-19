## Mo's own task queue, served over MCP. Agents lease a task, check in while
## working, and report it done or failed. A change to anything compiled in,
## meaning this program, job-queue.mo, lib/ or the compiler's output, refuses the
## saved data of a running queue.
module Main
expose JobFilter(..), Page(..), PageError(..), Stats(..), SubmitError(..)
use JobQueue{Queue, Job, QueueError}

max_payload_bytes = 2048
max_jobs = 5000
max_page_jobs = 100
lease_length = 30.minutes
full_advice = "This limit counts every job ever submitted and never falls, so retrying cannot help. Tell a person."

service
  state Queue = JobQueue.empty
  tools submit, lease, heartbeat, acknowledge, fail, list, stats
  every 1.minute expire
  token from env "QUEUE_TOKEN"

  promise "no job is ever lost"
    ids(before).all?(fn(id) ids(after).contains?(id) end)
    allows before: one_job, after: one_job, now: t0
    forbids before: one_job, after: JobQueue.empty, now: t0
  end

  promise "a finished job stays as it finished"
    JobQueue.jobs(before).all?(
      fn(job) not finished?(job) or JobQueue.jobs(after).contains?(job) end
    )
    allows before: done_job, after: done_job, now: t0
    forbids before: done_job, after: one_job, now: t0
  end

  promise "a lease ends at most 30 minutes from now"
    JobQueue.jobs(after).all?(
      fn(job) job.state is JobQueue.Leased(until: until) implies until <= now + lease_length end
    )
    allows before: one_job, after: leased_job, now: t0
    forbids before: one_job, after: leased_job, now: t0 - 1.hour
  end

  promise "a submitted job is in the queue, ready" on submit
    reply is Ok(id) implies
      JobQueue.jobs(after).contains?(
        Job(id: id, payload: payload, attempts: 0, state: JobQueue.Ready)
      )
    allows before: JobQueue.empty, payload: "x", reply: Ok(1), after: one_job, now: t0
    forbids before: JobQueue.empty, payload: "x", reply: Ok(1), after: JobQueue.empty, now: t0
  end

  promise "submit refuses only an oversized payload or a full queue" on submit
    reply is Error(PayloadTooLarge(limit: limit, bytes: bytes)) implies
      limit == max_payload_bytes and bytes == payload.byte_count and bytes > limit
    reply is Error(QueueFull(limit: limit, advice: _)) implies
      limit == max_jobs and JobQueue.jobs(before).size >= limit
    reply is Ok(_) implies
      payload.byte_count <= max_payload_bytes and JobQueue.jobs(before).size < max_jobs
    allows before: JobQueue.empty, payload: "x", reply: Ok(1), after: one_job, now: t0
    forbids before: JobQueue.empty, payload: "x", reply: Error(
      QueueFull(limit: max_jobs, advice: full_advice)
    ), after: JobQueue.empty, now: t0
  end

  promise "a lease takes the oldest ready job for 30 minutes and counts an attempt" on lease
    reply is Ok(Some(job)) implies
      JobQueue.jobs(before).find(fn(old) old.state == JobQueue.Ready end) is Some(old) and
        job ==
          Job(
            id: old.id,
            payload: old.payload,
            attempts: old.attempts + 1,
            state: JobQueue.Leased(until: now + lease_length)
          ) and
        JobQueue.jobs(after).contains?(job)
    reply is Ok(None) implies JobQueue.jobs(before).all?(fn(old) old.state != JobQueue.Ready end)
    allows before: one_job, reply: Ok(Some(first_lease)), after: leased_job, now: t0
    forbids before: one_job, reply: Ok(None), after: one_job, now: t0
  end

  promise "a heartbeat renews only your current lease, for 30 minutes from now" on heartbeat
    reply is Ok(_) implies current?(before, now, id, attempt)
    reply is Ok(_) implies
      job_of(before, id) is Some(old) and
        JobQueue.jobs(after).contains?(
          Job(
            id: id,
            payload: old.payload,
            attempts: attempt,
            state: JobQueue.Leased(until: now + lease_length)
          )
        )
    allows before: leased_job, id: 1, attempt: 1, reply: Ok(Unit), after: leased_job, now: t0
    forbids before: leased_job, id: 1, attempt: 2, reply: Ok(Unit), after: leased_job, now: t0
  end

  promise "acknowledging finishes only your current lease" on acknowledge
    reply is Ok(_) implies current?(before, now, id, attempt)
    reply is Ok(_) implies
      job_of(before, id) is Some(old) and
        JobQueue.jobs(after).contains?(
          Job(id: id, payload: old.payload, attempts: attempt, state: JobQueue.Done)
        )
    allows before: leased_job, id: 1, attempt: 1, reply: Ok(Unit), after: done_job, now: t0
    forbids before: leased_job, id: 1, attempt: 1, reply: Ok(Unit), after: leased_job, now: t0
  end

  promise "failing gives back only your current lease, ready or dead on its third attempt" on fail
    reply is Ok(_) implies current?(before, now, id, attempt)
    reply is Ok(_) implies
      job_of(before, id) is Some(old) and
        JobQueue.jobs(after).contains?(
          Job(
            id: id,
            payload: old.payload,
            attempts: attempt,
            state: if attempt >= 3: JobQueue.Dead else: JobQueue.Ready
          )
        )
    allows before: one_job, id: 1, attempt: 1, reply: Error(JobQueue.NotLeased(id: 1)), after: one_job, now: t0
    forbids before: one_job, id: 1, attempt: 1, reply: Ok(Unit), after: one_job, now: t0
  end

  promise "stats counts every job by its state" on stats
    reply.total == JobQueue.jobs(before).size
    reply.ready == matching(before, Ready, 0).size and
      reply.leased == matching(before, Leased, 0).size
    reply.done == matching(before, Done, 0).size and reply.dead == matching(before, Dead, 0).size
    reply.limit == max_jobs and reply.now == now
    allows before: one_job, reply: Stats(
      total: 1,
      ready: 1,
      leased: 0,
      done: 0,
      dead: 0,
      limit: max_jobs,
      now: t0
    ), after: one_job, now: t0
    forbids before: done_job, reply: Stats(
      total: 1,
      ready: 0,
      leased: 0,
      done: 0,
      dead: 0,
      limit: max_jobs,
      now: t0
    ), after: done_job, now: t0
  end
end

enum SubmitError
  PayloadTooLarge(limit: Int, bytes: Int)
  QueueFull(limit: Int, advice: Text)
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

## Adds a task and returns its id. Every Int in this service, in arguments and
## replies, is a JSON string.
## `payload` is free text handed to a worker unchanged, at most 2,048 bytes. Give
## a short brief and say where the rest is. It is kept for good and any token
## holder can read it, so keep secrets out.
## PayloadTooLarge gives the limit and the size sent: shorten it. HTTP 413 means
## the request body passed 16 KiB, so the payload never reached the queue:
## shorten it.
## QueueFull: the limit of 5,000 counts every job ever submitted, including Done
## and Dead, and never falls. Retrying cannot help, so tell a person.
## Not safe to retry blindly: a retry after a lost reply or store_failed makes a
## second job, so check `list` first.
## Never repeat a call that got handler_failed, work_limit or memory_limit. Five
## of those in 60 seconds restart the service, and three restarts stop it for
## everyone until a person starts it.
fn submit(queue: Queue, _now: Time, payload: Text) : Result((Queue, Int), SubmitError)
  examples
    submit(submitted(done_job, max_jobs - 1), t0, "x") ==
      Error(QueueFull(limit: max_jobs, advice: full_advice))
    submit(JobQueue.empty, t0, "x".repeat(max_payload_bytes + 1)) ==
      Error(PayloadTooLarge(limit: max_payload_bytes, bytes: max_payload_bytes + 1))
    submit(JobQueue.empty, t0, "x".repeat(max_payload_bytes)).map(fn(pair) pair.1 end) == Ok(1)
  end

  bytes = payload.byte_count
  return Error(PayloadTooLarge(limit: max_payload_bytes, bytes: bytes)) if bytes > max_payload_bytes
  return Error(QueueFull(limit: max_jobs, advice: full_advice)) if JobQueue.jobs(queue).size >=
    max_jobs
  Ok(JobQueue.submit(queue, payload))
end

## Leases the oldest ready task for 30 minutes and counts an attempt. Replies
## {"case":"None"} when nothing is ready, and nothing will notify you. To look
## without taking anything, use `list`. Wait a minute before calling this again.
## {"case":"Some","value":job} gives you the job. A job is {"id","payload",
## "attempts","state"}, and `state` is {"case":"Ready"}, {"case":"Leased","fields":
## {"until":time}}, {"case":"Done"} or {"case":"Dead"}. Pass `job.id` as `id` and
## `job.attempts` as `attempt` to the other tools, and keep both somewhere that
## survives losing your context: no tool tells you which job is yours. The lease ends at
## `job.state.fields.until`. Call `heartbeat` about every 10 minutes.
## If `job.attempts` is above 1, an earlier attempt ran and may have left partial
## work, so check before starting. The third attempt that fails or expires makes
## the job Dead for good; a coordinator re-submits it as a new job or tells a
## person.
## Never hold a lease while waiting on a person, such as for a PR review: the
## lease runs out and another worker repeats the task. Acknowledge at hand-off.
## Not safe to retry: after a lost reply the job stays held by nobody until the
## lease ends, and one attempt is spent.
## A tool's own refusal is {"error":{"kind":"application","value":{"case":...}}}.
## A runtime error is {"error":{"kind":"runtime","code":...,"message":...,
## "effect":...}}, where `effect` says what this call did to the queue: none,
## applied or unknown. It does not say whether a retry is safe.
## What to do about runtime errors, on every tool:
## - busy, rate_limited: wait, then retry.
## - invalid_arguments: fix the call; do not repeat it.
## - handler_failed, work_limit, memory_limit: never repeat the call, and tell a
##   person. Five of those in 60 seconds restart the service, and three restarts
##   stop it for everyone until a person starts it. With effect none, your lease
##   still ends when it did, so keep working inside it.
## - store_failed, or no reply at all: the outcome is unknown, so treat it as a
##   lost reply for that tool.
## - stopped: the service is down until a person starts it. Stop calling and keep
##   your id and attempt.
## - response_limit: on `list`, lower `limit`. On a tool that changes the queue,
##   the change was applied.
fn lease(queue: Queue, now: Time) : Result((Queue, Option(Job)), QueueError)
  JobQueue.lease(queue, now, lease_length)
end

## Renews your lease for 30 minutes from now without counting an attempt. Call it
## about every 10 minutes while working, and before any step that may block for
## over 20 minutes. `id` is `job.id` and `attempt` is `job.attempts`, both JSON
## strings from `lease`. Safe to retry.
## NotLeased: read the job with `list`, using filter {"case":"AllJobs"}, `after`
## id - 1 and `limit` 1, then compare. Done with your attempt: your acknowledge
## landed, so that is success. Ready or Leased with your attempt: your lease ran
## out, so stop. Dead: stop and report. More attempts than yours: a later attempt
## owns the job, so stop.
## NoSuchJob: the id is wrong; do not retry.
## Never repeat a call that got handler_failed, work_limit or memory_limit: it
## can stop the service for everyone.
fn heartbeat(queue: Queue, now: Time, id: Int, attempt: Int) : Result((Queue, Unit), QueueError)
  after = try JobQueue.heartbeat(queue, now, id, attempt)
  Ok((after, Unit))
end

## Marks your leased job Done. A task ends at hand-off: acknowledge when your PR
## opens or your work is delivered, not after a person reviews it. `id` is
## `job.id` and `attempt` is `job.attempts`, both JSON strings from `lease`. Safe
## to retry.
## NotLeased does not mean your work failed: read the job with `list`, using
## filter {"case":"AllJobs"}, `after` id - 1 and `limit` 1, then compare. Done
## with your attempt: your earlier acknowledge landed, so that is success. Ready
## or Leased with your attempt: your lease ran out, so stop and tell the
## coordinator what you delivered. Dead: stop and report. More attempts than
## yours: a later attempt owns the job, so stop and do not acknowledge.
## NoSuchJob: the id is wrong; do not retry.
## Never repeat a call that got handler_failed, work_limit or memory_limit: it
## can stop the service for everyone.
fn acknowledge(queue: Queue, now: Time, id: Int, attempt: Int) : Result((Queue, Unit), QueueError)
  after = try JobQueue.acknowledge(queue, now, id, attempt)
  Ok((after, Unit))
end

## Gives your leased job back when you cannot finish it. It goes back to Ready,
## or to Dead on its third attempt, and a Dead job is never retried. The queue
## may hand the same job to the next `lease`. `id` is `job.id` and `attempt` is
## `job.attempts`, both JSON strings from `lease`. Safe to retry.
## NotLeased: read the job with `list`, using filter {"case":"AllJobs"}, `after`
## id - 1 and `limit` 1, then compare. Done with your attempt: an earlier
## acknowledge landed. Ready or Leased with your attempt: your lease ran out or
## your earlier fail landed, so stop. Dead: stop and report. More attempts than
## yours: a later attempt owns the job, so stop.
## NoSuchJob: the id is wrong; do not retry.
## Never repeat a call that got handler_failed, work_limit or memory_limit: it
## can stop the service for everyone.
fn fail(queue: Queue, now: Time, id: Int, attempt: Int) : Result((Queue, Unit), QueueError)
  after = try JobQueue.fail(queue, now, id, attempt)
  Ok((after, Unit))
end

## Reads jobs in id order with their payloads, and changes nothing. Safe to
## retry. It is the only way to see the queue, and nothing announces a dead job,
## so a coordinator should list Dead jobs at the start of each cycle.
## All three arguments are required. `filter` is {"case":"X"}, where X is AllJobs,
## Ready, Leased, Done or Dead. `after` is an id: jobs with larger ids are listed,
## so start at "0". `limit` is 1 to 100.
## The reply has `jobs`, `next` and `now`. A job is {"id","payload","attempts",
## "state"}, and `state` is {"case":"Ready"}, {"case":"Leased","fields":
## {"until":time}}, {"case":"Done"} or {"case":"Dead"}. `next` is {"case":"None"}
## on the last page; otherwise pass `next.value` as `after`. `now` is the service's
## clock, for reading each lease's `until`.
## To read one job, use filter {"case":"AllJobs"}, `after` id - 1 and `limit` 1.
## To count jobs without reading payloads, use `stats`.
## response_limit: lower `limit` and call again. A limit of 1 always fits.
## BadPage: fix the arguments.
## Never pass a job's id and attempts from here to `heartbeat`, `acknowledge` or
## `fail` unless `lease` gave that job to you.
## A dead job's only way back is `submit` with its payload, which makes a new job;
## name the old id in the new payload, because the dead record stays listed.
## Never repeat a call that got handler_failed, work_limit or memory_limit: it
## can stop the service for everyone.
fn list(queue: Queue, now: Time, filter: JobFilter, after: Int, limit: Int) : Result(
  Page,
  PageError
)
  promise "a page lists, in id order, the first jobs after `after` that its filter shows"
    (result is Error(_)) == (after < 0 or limit < 1 or limit > max_page_jobs)
    result is Ok(page) implies page.jobs == matching(queue, filter, after).take(limit)
    result is Ok(page) implies
      page.next == (
        if matching(queue, filter, after).size > limit: page.jobs.last.map(fn(job) job.id end) else: None
      )
    result is Ok(page) implies page.now == now
    allows queue: one_job, now: t0, filter: AllJobs, after: 0, limit: 1, result: Ok(
      Page(jobs: JobQueue.jobs(one_job), next: None, now: t0)
    )
    forbids queue: one_job, now: t0, filter: Done, after: 0, limit: 1, result: Ok(
      Page(jobs: JobQueue.jobs(one_job), next: None, now: t0)
    )
  end

  examples
    list(three_jobs, t0, AllJobs, 1, 1).map(fn(page) (ids_of(page.jobs), page.next) end) ==
      Ok(([2], Some(2)))
    list(three_jobs, t0, Done, 0, 100).map(fn(page) (ids_of(page.jobs), page.next) end) ==
      Ok(([1], None))
    list(three_jobs, t0, AllJobs, 0, 0) ==
      Error(BadPage(after: 0, limit: 0, max_limit: max_page_jobs))
  end

  if after < 0 or limit < 1 or limit > max_page_jobs
    return Error(BadPage(after: after, limit: limit, max_limit: max_page_jobs))
  end
  matching = JobQueue.jobs(queue).filter(fn(job) job.id > after and shown?(filter, job) end)
  page = matching.take(limit)
  next = if matching.size > limit: page.last.map(fn(job) job.id end) else: None
  Ok(Page(jobs: page, next: next, now: now))
end

## Counts the jobs in each state, and changes nothing. Safe to retry, and cheap:
## it returns no payloads. Every Int in the reply is a JSON string.
## `total` counts every job ever submitted, including Done and Dead, because jobs
## are never removed. `submit` refuses once `total` reaches `limit`, and that never
## changes, so tell a person when `total` gets close to `limit`.
## `leased` includes leases whose time has passed but that the one-minute expiry
## tick has not returned yet. `now` is the service's clock.
## A coordinator can call this at the start of each cycle, then list Dead jobs
## with `list` when `dead` has grown.
## Never repeat a call that got handler_failed, work_limit or memory_limit: it
## can stop the service for everyone.
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
  total = ready + leased + done + dead
  Stats(
    total: total,
    ready: ready,
    leased: leased,
    done: done,
    dead: dead,
    limit: max_jobs,
    now: now
  )
end

fn expire(queue: Queue, now: Time) : Queue
  examples
    expire(leased_job, t0 + lease_length - 1.second) == leased_job
    JobQueue.jobs(expire(leased_job, t0 + lease_length)) == [
      Job(id: 1, payload: "x", attempts: 1, state: JobQueue.Ready)
    ]
  end

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

fn ids(queue: Queue) : List(Int)
  JobQueue.jobs(queue).map(fn(job) job.id end)
end

fn finished?(job: Job) : Bool
  job.state == JobQueue.Done or job.state == JobQueue.Dead
end

t0 = Time.parse("2026-10-12T09:00:00Z") or stop "the time is written correctly"
one_job = JobQueue.submit(JobQueue.empty, "x").0
leased_job = case JobQueue.lease(one_job, t0, lease_length)
  Ok((queue, _)): queue
  Error(_): stop "a ready job can be leased"
end
done_job = case JobQueue.acknowledge(leased_job, t0, 1, 1)
  Ok(queue): queue
  Error(_): stop "a leased job can be acknowledged"
end
first_lease = Job(
  id: 1,
  payload: "x",
  attempts: 1,
  state: JobQueue.Leased(until: t0 + lease_length)
)
three_jobs = submitted(done_job, 2)

fn submitted(queue: Queue, count: Int) : Queue
  var after = queue
  for _ in 0..count
    after = JobQueue.submit(after, "x").0
  end
  after
end

fn ids_of(jobs: List(Job)) : List(Int)
  jobs.map(fn(job) job.id end)
end

fn job_of(queue: Queue, id: Int) : Option(Job)
  JobQueue.jobs(queue).find(fn(job) job.id == id end)
end

fn current?(queue: Queue, now: Time, id: Int, attempt: Int) : Bool
  job_of(queue, id) is Some(job) and
    job.attempts == attempt and
    job.state is JobQueue.Leased(until: until) and
    until > now
end

fn matching(queue: Queue, filter: JobFilter, after: Int) : List(Job)
  JobQueue.jobs(queue).filter(fn(job) job.id > after and wanted?(filter, job) end)
end

fn wanted?(filter: JobFilter, job: Job) : Bool
  case filter
    AllJobs: true
    Ready: job.state == JobQueue.Ready
    Leased: job.state is JobQueue.Leased(until: _)
    Done: job.state == JobQueue.Done
    Dead: job.state == JobQueue.Dead
  end
end
