## Two services in one program, for the recovery checker: each gets its own check.
module Main
use Tally

service
  state Int = 0
  tools add, total
  token from env "MAIN_TOKEN"
end

fn add(state: Int, _now: Time, n: Bool) : (Int, Int)
  next = if n: state + Tally.step else: state
  (next, next)
end

fn total(state: Int, _now: Time) : List(Int)
  [state]
end
