module Main

service
  state Int = 0
  tools add
  token from env "COUNTER_TOKEN"

  promise "an addition never shrinks the count" on add
    after >= before
    allows before: 1, reply: 2, after: 2, now: t0
    forbids before: 1, reply: 0, after: 0, now: t0
  end
end

fn add(state: Int, _now: Time, after: Int) : (Int, Int)
  (state + after.abs, state + after.abs)
end

t0 = Time.parse("2026-01-01T00:00:00Z") or stop "time"
