module Main
use Api

struct Quota
  used: Int
end

enum QuotaError
  Denied
end

actor Limiter
  state Quota = Quota(used: 0)
  calls acquire, used, reset_quota
  every 30.seconds decay

  promise
    after.used >= 0
  end

  promise on acquire
    after.used >= before.used
  end
end

fn acquire(state: Quota, _now: Time, cost: Int) : Result((Quota, Int), QuotaError)
  return Error(Denied) if cost < 0 or state.used + cost > 5
  next = state.used + cost
  Ok((Quota(used: next), next))
end

fn used(state: Quota, _now: Time) : Int
  state.used
end

fn reset_quota(_state: Quota, _now: Time) : (Quota, Unit)
  (Quota(used: 0), Unit)
end

fn decay(state: Quota, _now: Time) : Quota
  if state.used > 0: Quota(used: state.used - 1) else: state
end

api
  actor lim: Limiter
  post "/acquire/:cost" post_acquire
  get "/used" get_used
  status Denied = 429
end

fn post_acquire(lim: Limiter, cost: Int) : Result(Int, QuotaError)
  lim.acquire(cost)
end

fn get_used(lim: Limiter) : Int
  lim.used
end

test "pure handler functions match Lean"
  t0 = Time.parse("2026-01-01T00:00:00Z") or stop "bad time"
  s0 = Quota(used: 0)
  assert acquire(s0, t0, 3) == Ok((Quota(used: 3), 3))
  assert acquire(Quota(used: 4), t0, 2) == Error(Denied)
  assert decay(Quota(used: 2), t0) == Quota(used: 1)
end

test "actor handle dot calls and timer advance"
  t0 = Time.parse("2026-01-01T00:00:00Z") or stop "bad time"
  lim = Actor.start_at(Limiter, t0)
  assert lim.acquire(3) == Ok(3)
  assert lim.acquire(3) == Error(Denied)
  assert lim.used == 3
  Actor.advance(lim, 30.seconds)
  assert lim.used == 2
  lim.reset_quota
  assert lim.used == 0
end

test "actor composition with ApiTest.send and Actor.send"
  lim = Actor.start(Limiter)
  assert ApiTest.send(Main, Api.post("/acquire/4"), lim).status == 200
  assert ApiTest.send(Main, Api.post("/acquire/2"), lim).status == 429
  assert ApiTest.send(Main, Api.get("/used"), lim).text == "4"
  Actor.crash(lim)
  assert Actor.send(lim, Limiter.used) == Error(ServiceDown)
  Actor.restart(lim)
  assert Actor.send(lim, Limiter.used) == Ok(3)
end
