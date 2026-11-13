module Main

fn submit(queue: Queue, _now: Time, payload: Text) : Result((Queue, Int), SubmitError)
  bytes = payload.byte_count
  return Error(PayloadTooLarge(limit: max_payload_bytes, bytes: bytes)) if bytes > max_payload_bytes
  open = JobQueue.jobs(queue).filter(fn(job) not finished?(job) end)
  return Error(QueueFull(limit: max_jobs, advice: full_advice)) if open.size >= max_jobs
  Ok(JobQueue.submit(queue, payload))
end
