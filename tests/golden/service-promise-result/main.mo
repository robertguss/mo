## A tool returning Result((State, R), E): its promises read reply as Result(R, E). The first
## holds on every reply; the second is false for every refusal, which leaves the balance alone.
module Main

service
  state Int = 10
  tools withdraw
  token from env "BANK_TOKEN"

  promise "a withdrawal pays out what it took" on withdraw
    reply is Ok(paid) implies paid == before - after
    allows before: 10, amount: 3, reply: Ok(3), after: 7, now: t0
    forbids before: 10, amount: 3, reply: Ok(3), after: 10, now: t0
  end

  promise "a refused withdrawal empties the account" on withdraw
    reply is Error(_) implies after == 0
    allows before: 10, amount: 11, reply: Error(Overdrawn(balance: 10)), after: 0, now: t0
    forbids before: 10, amount: 11, reply: Error(Overdrawn(balance: 10)), after: 10, now: t0
  end
end

enum Refusal
  Overdrawn(balance: Int)
end

fn withdraw(balance: Int, _now: Time, amount: Int) : Result((Int, Int), Refusal)
  taken = amount.abs
  return Error(Overdrawn(balance: balance)) if taken > balance
  Ok((balance - taken, taken))
end

t0 = Time.parse("2026-01-01T00:00:00Z") or stop "time"
