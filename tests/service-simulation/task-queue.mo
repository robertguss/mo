module TaskQueue
use JobQueue

service
  state JobQueue.Queue = JobQueue.empty
  tools submit, lease
  token from env "TOKEN"
end

fn submit(state: JobQueue.Queue, _now: Time, payload: Text) : (JobQueue.Queue, Int)
  JobQueue.submit(state, payload)
end

fn lease(state: JobQueue.Queue, now: Time, length: Duration) : Result(
  (JobQueue.Queue, Option(JobQueue.Job)),
  JobQueue.QueueError
)
  JobQueue.lease(state, now, length)
end
