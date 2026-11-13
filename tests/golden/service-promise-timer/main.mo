## A credit balance that the hourly fee can drive below zero: a timer firing breaks the step
## promise, so the failing step is a clock advance. The tool promise holds.
module Main

service
  state Int = 0
  tools top_up
  every 1.hour charge
  token from env "CREDIT_TOKEN"

  promise "the balance never goes below zero"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end

  promise "a top up adds what it says" on top_up
    reply == after and after >= before
    allows before: 1, amount: 2, reply: 3, after: 3, now: t0
    forbids before: 1, amount: 2, reply: 3, after: 0, now: t0
  end
end

fn top_up(balance: Int, _now: Time, amount: Int) : (Int, Int)
  added = balance + amount.abs
  (added, added)
end

fn charge(balance: Int, _now: Time) : Int
  return balance if balance == 0
  balance - 5
end

t0 = Time.parse("2026-01-01T00:00:00Z") or stop "time"
