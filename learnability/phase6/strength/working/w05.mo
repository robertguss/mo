module JobQueue

fn expire(queue: Queue, now: Time) : Queue
  var after = queue
  for job in queue.jobs.values
    if job.state is Leased(until: until) and until <= now
      var returned = job
      returned.state = Ready
      after = put(after, returned)
    end
  end
  after
end
