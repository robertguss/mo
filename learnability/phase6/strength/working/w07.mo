module Main

fn stats(queue: Queue, now: Time) : Stats
  var ready = 0
  var leased = 0
  var done = 0
  var dead = 0
  for job in JobQueue.jobs(queue)
    case job.state
      JobQueue.Ready: ready += 1
      JobQueue.Leased(until: _): leased += 1
      JobQueue.Done | JobQueue.Dead: done += 1
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
