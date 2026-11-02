## Mo's own task queue, served over MCP. Agents lease a task, check in while
## working, and report it done or failed. A change to anything compiled in,
## meaning this program, job-queue.mo, lib/ or the compiler's output, refuses the
## saved data of a running queue.
module Main
expose JobFilter(..), Page(..), PageError(..), SubmitError(..)
use JobQueue{Queue, Job, QueueError}

max_payload_bytes = 2048
max_jobs = 5000
max_page_jobs = 100
lease_length = 30.minutes
full_advice = "This limit counts every job ever submitted and never falls, so retrying cannot help. Tell a person."

service
  state Queue = JobQueue.empty
  tools submit, lease, heartbeat, acknowledge, fail, list
  every 1.minute expire
  token from env "QUEUE_TOKEN"
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
  if after < 0 or limit < 1 or limit > max_page_jobs
    return Error(BadPage(after: after, limit: limit, max_limit: max_page_jobs))
  end
  matching = JobQueue.jobs(queue).filter(fn(job) job.id > after and shown?(filter, job) end)
  page = matching.take(limit)
  next = if matching.size > limit: page.last.map(fn(job) job.id end) else: None
  Ok(Page(jobs: page, next: next, now: now))
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
