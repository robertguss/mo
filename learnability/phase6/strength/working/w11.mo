module Main

fn shown?(filter: JobFilter, job: Job) : Bool
  case filter
    AllJobs: true
    Ready: job.state == JobQueue.Ready
    Leased: job.state is JobQueue.Leased(until: _)
    Done: finished?(job)
    Dead: job.state == JobQueue.Dead
  end
end
