## Two promises of one service fail in the same run: each is shrunk and reported on its own, and
## the search goes on after the first for the second.
module Main

service
  state Int = 0
  tools add
  token from env "COUNTER_TOKEN"

  promise "an addition never lowers the count" on add
    after >= before
    allows before: 1, amount: 2, reply: Unit, after: 3, now: t0
    forbids before: 3, amount: -2, reply: Unit, after: 1, now: t0
  end

  promise "the count stays under a hundred"
    after < 100
    allows before: 1, after: 99, now: t0
    forbids before: 99, after: 100, now: t0
  end
end

fn add(state: Int, _now: Time, amount: Int) : (Int, Unit)
  (state + amount, Unit)
end

t0 = Time.parse("2026-01-01T00:00:00Z") or stop "time"
