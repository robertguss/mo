## In-memory structured log batching actor for `mo-pulse`: buffers incoming log entries
## and drains them atomically for bulk SQLite persistence and indexing.
module Batch
expose BatchWriter, BufferedLog(..), enqueue_log, drain_logs, pending_count

struct BufferedLog
  service_name: Text
  level: Text
  message: Text
end

struct BatchState
  queue: List(BufferedLog)
  total_enqueued: Int
end

actor BatchWriter
  state BatchState = BatchState(queue: [], total_enqueued: 0)
  calls enqueue_log, drain_logs, pending_count

  promise
    after.total_enqueued >= before.total_enqueued
  end

  promise on drain_logs
    after.queue.empty?
  end
end

fn enqueue_log(state: BatchState, _now: Time, item: BufferedLog) : (BatchState, Int)
  capped = if state.queue.size >= 500: state.queue.drop(1) else: state.queue
  next_q = capped.append(item)
  next = BatchState(queue: next_q, total_enqueued: state.total_enqueued.saturating_add(1))
  (next, next_q.size)
end

fn drain_logs(state: BatchState, _now: Time) : (BatchState, List(BufferedLog))
  (BatchState(queue: [], total_enqueued: state.total_enqueued), state.queue)
end

fn pending_count(state: BatchState, _now: Time) : Int
  state.queue.size
end
