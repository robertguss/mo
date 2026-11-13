module Main

service
  state Int = 0
  tools add
  token from env "COUNTER_TOKEN"

  promise "a removal never adds" on remove
    after <= before
    allows before: 1, amount: 1, reply: 0, after: 0, now: t0
    forbids before: 1, amount: 1, reply: 2, after: 2, now: t0
  end
end

fn add(state: Int, _now: Time, amount: Int) : (Int, Int)
  (state + amount, state + amount)
end

fn remove(state: Int, amount: Int) : Int
  state - amount
end

t0 = Time.parse("2026-01-01T00:00:00Z") or stop "time"
