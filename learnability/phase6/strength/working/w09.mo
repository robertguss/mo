module Main

fn fail(queue: Queue, now: Time, id: Int, attempt: Int) : Result((Queue, Unit), QueueError)
  case JobQueue.fail(queue, now, id, attempt)
    Ok(after): Ok((after, Unit))
    Error(JobQueue.NotLeased(id: _)): Ok((queue, Unit))
    Error(problem): Error(problem)
  end
end
