module Main
expose add

service
  state Int = 0
  tools add, twice, peek, bump
  token from env "COUNTER_TOKEN"

  promise "the count never falls"
    after >= before
    allows before: 1, after: 3, now: t0
    forbids before: 3, after: 1, now: t0
  end

  promise "an addition adds its amount's size" on add
    after == before + amount.abs
    allows before: 1, amount: -2, reply: Unit, after: 3, now: t0
    forbids before: 1, amount: -2, reply: Unit, after: -1, now: t0
  end

  promise "twice doubles the count" on twice
    after == before * 2
    allows before: 2, reply: Unit, after: 4, now: t0
    forbids before: 2, reply: Unit, after: 3, now: t0
  end
end

fn add(state: Int, _now: Time, amount: Int) : (Int, Unit)
  (state + amount.abs, Unit)
end

fn twice(state: Int, _now: Time) : (Int, Unit)
  (state * 2, Unit)
end

fn peek(state: Int, _now: Time) : List(Int)
  [state]
end

fn bump(state: Int, _now: Time) : (Int, Unit)
  examples
    bump(1, t0) == (2, Unit)
  end

  (state + 1, Unit)
end

t0 = Time.parse("2026-01-01T00:00:00Z") or stop "time"
