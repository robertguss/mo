module Main

fn list(queue: Queue, now: Time, filter: JobFilter, after: Int, limit: Int) : Result(
  Page,
  PageError
)
  if after < 0 or limit < 1 or limit > max_page_jobs
    return Error(BadPage(after: after, limit: limit, max_limit: max_page_jobs))
  end
  matching = JobQueue.jobs(queue).filter(fn(job) job.id >= after and shown?(filter, job) end)
  page = matching.take(limit)
  next = if matching.size > limit: page.last.map(fn(job) job.id end) else: None
  Ok(Page(jobs: page, next: next, now: now))
end
