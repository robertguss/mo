## A value queue. Keep the returned queue after every operation.
## Oldest means submission order, not the earliest ready_at.
module Answers.JobQueue
expose Queue, Job(..), Status(..), Lease, QueueError(..), empty, jobs,
  submit, lease, acknowledge, fail, expire_leases,
  job_id, payload, attempt, expires_at

enum Status
  Ready
  Leased(Time)
  Acknowledged
  Dead
end

struct Job
  id: BigInt
  payload: Text
  ready_at: Time
  attempts: Int
  status: Status
end

## Hidden fields prevent callers from constructing an invalid queue.
struct Queue
  next_id: BigInt
  entries: Map(BigInt, Job)
end

## Each attempt has its own receipt; an old receipt cannot finish a retry.
struct Lease
  id: BigInt
  payload: Text
  attempt: Int
  expires_at: Time
end

enum QueueError
  InvalidLeaseDuration
  DeadlineOutOfRange
  UnknownJob
  StaleLease
  LeaseExpired
end

fn empty() : Queue
  Queue(next_id: 1.to_big, entries: {})
end

fn jobs(queue: Queue) : List(Job)
  queue.entries.values
end

fn job_id(receipt: Lease) : BigInt
  receipt.id
end

fn payload(receipt: Lease) : Text
  receipt.payload
end

fn attempt(receipt: Lease) : Int
  receipt.attempt
end

fn expires_at(receipt: Lease) : Time
  receipt.expires_at
end

fn submit(queue: Queue, body: Text, ready_at: Time) : (Queue, BigInt)
  promise "submission preserves every old job and adds exactly one fresh job"
    identities(result.0) == identities(queue).append((queue.next_id, body))
    result.1 == queue.next_id
    result.0.next_id == queue.next_id + 1.to_big
    queue_valid?(result.0)
    result.0.entries.get(result.1) is Some(added) and
      added.ready_at == ready_at and added.attempts == 0 and added.status is Ready
    allows queue: empty(), body: "alpha", ready_at: example_time,
      result: (example_queue(Ready, 0, example_time), 1.to_big)
    forbids queue: empty(), body: "alpha", ready_at: example_time,
      result: (Queue(next_id: 2.to_big, entries: {}), 1.to_big)
  end

  entry = Job(
    id: queue.next_id,
    payload: body,
    ready_at: ready_at,
    attempts: 0,
    status: Ready
  )
  updated = Queue(
    next_id: queue.next_id + 1.to_big,
    entries: queue.entries.set(entry.id, entry)
  )
  (updated, entry.id)
end

## The deadline itself is expired: only now < deadline is live.
fn expire_leases(queue: Queue, now: Time) : Queue
  promise "expiry loses no job, keeps attempt counts, and leaves no expired lease"
    identities(result) == identities(queue)
    result.next_id == queue.next_id
    result.entries.values.map(fn(entry) entry.attempts end) ==
      queue.entries.values.map(fn(entry) entry.attempts end)
    queue_valid?(result)
    result.entries.values.all?(fn(entry)
      case entry.status
        Leased(deadline): deadline > now
        Ready | Acknowledged | Dead: true
      end
    end)
    allows queue: example_queue(Leased(example_time), 3, example_time),
      now: example_time, result: example_queue(Dead, 3, example_time)
    forbids queue: example_queue(Leased(example_time), 3, example_time),
      now: example_time, result: Queue(next_id: 2.to_big, entries: {})
  end

  var updated = queue
  for (id, entry) in queue.entries
    if entry.status is Leased(deadline) and deadline <= now
      var expired = entry
      expired.status = if entry.attempts >= 3: Dead else: Ready
      expired.ready_at = now
      updated.entries = updated.entries.set(id, expired)
    end
  end
  updated
end

## Invalid requests leave the queue unchanged. Valid requests expire leases
## first, even when no ready job can be returned.
fn lease(queue: Queue, now: Time, duration: Duration) : (Queue, Result(Option(Lease), QueueError))
  promise "leasing loses no job, picks the oldest eligible job, and never starts a fourth attempt"
    identities(result.0) == identities(queue)
    result.0.next_id == queue.next_id
    queue_valid?(result.0)
    case result.1
      Error(_): result.0 == queue
      Ok(None): eligible_jobs(queue, now).empty?
      Ok(Some(issued)):
        eligible_jobs(queue, now).first is Some(oldest) and
          issued.id == oldest.id and issued.payload == oldest.payload and
          issued.attempt == oldest.attempts + 1 and issued.attempt <= 3 and
          now.checked_add(duration) == Some(issued.expires_at) and
          result.0.entries.get(issued.id) is Some(leased_job) and
          leased_job.status == Leased(issued.expires_at) and
          leased_job.attempts == issued.attempt
    end
    allows queue: example_queue(Ready, 0, example_time), now: example_time,
      duration: 1.seconds,
      result: (example_queue(Leased(example_time + 1.seconds), 1, example_time),
        Ok(Some(example_receipt(1, example_time + 1.seconds))))
    forbids queue: example_queue(Ready, 0, example_time), now: example_time,
      duration: 1.seconds, result: (Queue(next_id: 2.to_big, entries: {}), Ok(None))
  end

  return (queue, Error(InvalidLeaseDuration)) if duration <= 0.seconds
  deadline = case now.checked_add(duration)
    Some(value): value
    None: return (queue, Error(DeadlineOutOfRange))
  end
  expired = expire_leases(queue, now)
  candidate = expired.entries.values.find(fn(entry)
    entry.status is Ready and entry.ready_at <= now and entry.attempts < 3
  end)
  case candidate
    None: (expired, Ok(None))
    Some(entry):
      var leased = entry
      leased.attempts += 1
      leased.status = Leased(deadline)
      var updated = expired
      updated.entries = updated.entries.set(entry.id, leased)
      receipt = Lease(
        id: entry.id,
        payload: entry.payload,
        attempt: leased.attempts,
        expires_at: deadline
      )
      (updated, Ok(Some(receipt)))
  end
end

fn acknowledge(queue: Queue, receipt: Lease, now: Time) : (Queue, Result(Unit, QueueError))
  promise "acknowledgment retains all jobs, records success, and rejects an expired receipt"
    identities(result.0) == identities(queue)
    result.0.next_id == queue.next_id
    queue_valid?(result.0)
    case result.1
      Error(_): result.0 == queue
      Ok(_):
        now < receipt.expires_at and
          result.0.entries.get(receipt.id) is Some(finished_job) and
          finished_job.status is Acknowledged and
          finished_job.attempts == receipt.attempt
    end
    allows queue: example_queue(Leased(example_time + 1.seconds), 1, example_time),
      receipt: example_receipt(1, example_time + 1.seconds), now: example_time,
      result: (example_queue(Acknowledged, 1, example_time), Ok(Unit))
    forbids queue: example_queue(Leased(example_time + 1.seconds), 1, example_time),
      receipt: example_receipt(1, example_time + 1.seconds), now: example_time,
      result: (Queue(next_id: 2.to_big, entries: {}), Ok(Unit))
  end

  finish(queue, receipt, now, true)
end

## A failed live attempt becomes immediately ready, or dead after attempt 3.
fn fail(queue: Queue, receipt: Lease, now: Time) : (Queue, Result(Unit, QueueError))
  promise "failure retains all jobs and retries only jobs with fewer than three attempts"
    identities(result.0) == identities(queue)
    result.0.next_id == queue.next_id
    queue_valid?(result.0)
    case result.1
      Error(_): result.0 == queue
      Ok(_):
        now < receipt.expires_at and
          result.0.entries.get(receipt.id) is Some(finished_job) and
          finished_job.attempts == receipt.attempt and finished_job.ready_at == now and
          finished_job.status == (if receipt.attempt >= 3: Dead else: Ready)
    end
    allows queue: example_queue(Leased(example_time + 1.seconds), 3, example_time),
      receipt: example_receipt(3, example_time + 1.seconds), now: example_time,
      result: (example_queue(Dead, 3, example_time), Ok(Unit))
    forbids queue: example_queue(Leased(example_time + 1.seconds), 3, example_time),
      receipt: example_receipt(3, example_time + 1.seconds), now: example_time,
      result: (Queue(next_id: 2.to_big, entries: {}), Ok(Unit))
  end

  finish(queue, receipt, now, false)
end

fn finish(queue: Queue, receipt: Lease, now: Time, completed?: Bool) : (Queue, Result(Unit, QueueError))
  entry = case queue.entries.get(receipt.id)
    Some(value): value
    None: return (queue, Error(UnknownJob))
  end
  case entry.status
    Leased(deadline):
      return (queue, Error(StaleLease)) if entry.attempts != receipt.attempt
      return (queue, Error(StaleLease)) if deadline != receipt.expires_at
      return (queue, Error(StaleLease)) if entry.payload != receipt.payload
      return (queue, Error(LeaseExpired)) if now >= deadline
      var settled = entry
      settled.status = if completed?: Acknowledged else if entry.attempts >= 3: Dead else: Ready
      if not completed?
        settled.ready_at = now
      end
      var updated = queue
      updated.entries = updated.entries.set(entry.id, settled)
      (updated, Ok(Unit))
    Ready | Acknowledged | Dead: (queue, Error(StaleLease))
  end
end

## Comparing the ordered identity list preserves multiplicity as well as
## payloads and order; a count or a set of payloads alone would not suffice.
fn identities(queue: Queue) : List((BigInt, Text))
  queue.entries.values.map(fn(entry) (entry.id, entry.payload) end)
end

fn queue_valid?(queue: Queue) : Bool
  queue.next_id >= 1.to_big and queue.entries.entries.all?(fn(pair)
    entry = pair.1
    pair.0 == entry.id and entry.id >= 1.to_big and entry.id < queue.next_id and
      entry.attempts >= 0 and entry.attempts <= 3 and
      case entry.status
        Ready: entry.attempts < 3
        Leased(_): entry.attempts >= 1
        Acknowledged: entry.attempts >= 1
        Dead: entry.attempts == 3
      end
  end)
end

## Expired leases are eligible for another attempt without reordering them.
fn eligible_jobs(queue: Queue, now: Time) : List(Job)
  queue.entries.values.filter(fn(entry)
    entry.attempts < 3 and
      case entry.status
        Ready: entry.ready_at <= now
        Leased(deadline): deadline <= now
        Acknowledged | Dead: false
      end
  end)
end

# Concrete values for promise anchors, built without queue operations.
example_time = case Time.parse("2026-10-12T00:00:00Z")
  Some(value): value
  None: stop "the promise anchor is a valid RFC 3339 time"
end

fn example_queue(status: Status, attempts: Int, ready_at: Time) : Queue
  entry = Job(
    id: 1.to_big,
    payload: "alpha",
    ready_at: ready_at,
    attempts: attempts,
    status: status
  )
  Queue(next_id: 2.to_big, entries: {entry.id: entry})
end

fn example_receipt(attempts: Int, deadline: Time) : Lease
  Lease(id: 1.to_big, payload: "alpha", attempt: attempts, expires_at: deadline)
end
