
fn migrate(old: UpgradeOld.Queue) : Queue
  jobs = old.jobs.values.map(fn(job)
    state = case job.state
      UpgradeOld.Ready: JobQueue.Ready
      UpgradeOld.Leased(until: until): JobQueue.Leased(until: until)
      UpgradeOld.Done: JobQueue.Done
      UpgradeOld.Dead: JobQueue.Dead
    end
    (job.id, JobQueue.Job(id: job.id, payload: job.payload,
      attempts: job.attempts, state: state))
  end).to_map
  JobQueue.restore(jobs, old.next_id, 1)
end
