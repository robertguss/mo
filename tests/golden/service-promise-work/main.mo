## A promise body runs under the same work limit as a handler. One that runs past it is a bug,
## shown with the step it was judging.
module Main

service
  state Int = 0
  tools add
  token from env "COUNT_TOKEN"

  promise "the count is counted the long way"
    counted(after) == after
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
end

fn add(count: Int, _now: Time, n: Int) : (Int, Int)
  (count + n.abs, count + n.abs)
end

fn counted(n: Int) : Int
  var total = 0
  for _ in 0..n * 1000000
    total += 1
  end
  total / 1000000
end

t0 = Time.parse("2026-01-01T00:00:00Z") or stop "time"
