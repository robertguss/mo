module Main

fn expire(queue: Queue, now: Time) : Queue
  JobQueue.expire(queue, now + 1.minute)
end
