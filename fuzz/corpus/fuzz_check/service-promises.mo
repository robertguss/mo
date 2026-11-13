module Main

service
  state Int = 0
  tools increment
  every 1.minute tick
  token from env "COUNTER_TOKEN"

  promise "the count never falls"
    after >= before
    allows before: 1, after: 2, now: t0
    forbids before: 2, after: 1, now: t0
  end

  promise "an increment adds its amount" on increment
    reply implies after == before + amount
    allows before: 1, amount: 2, reply: true, after: 3, now: t0
    forbids before: 1, amount: 2, reply: true, after: 1, now: t0
  end
end

fn increment(state: Int, _now: Time, amount: Int) : (Int, Bool)
  (state + amount.abs, true)
end

fn tick(state: Int, _now: Time) : Int
  state
end

t0 = Time.parse("2026-01-01T00:00:00Z") or stop "time"
