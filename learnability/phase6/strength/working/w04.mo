module JobQueue

fn expire(queue: Queue, now: Time) : Queue
  var after = queue
  for job in queue.jobs.values
    if job.state is Leased(until: until) and until < now
      after = release(after, job)
    end
  end
  after
end
