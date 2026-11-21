module Main
use Api

struct Limiter
  hits: Map(Text, Int)
  checks: Int
end

enum AppError
  RateLimited
  NegativeDelta
end

actor RateLimiter
  state Limiter = Limiter(hits: {}, checks: 0)
  calls allow, current_checks, adjust
  every 1.minute clear_window

  promise
    after.checks >= 0
  end

  promise on allow
    after.checks == before.checks + 1
  end
end

fn allow(state: Limiter, _now: Time, key: Text) : (Limiter, Bool)
  used = state.hits.get(key) or 0
  if used >= 2
    (Limiter(hits: state.hits, checks: state.checks + 1), false)
  else
    (Limiter(hits: state.hits.set(key, used + 1), checks: state.checks + 1), true)
  end
end

fn current_checks(state: Limiter, _now: Time) : Int
  state.checks
end

fn adjust(state: Limiter, _now: Time, delta: Int) : Result((Limiter, Int), AppError)
  return Error(NegativeDelta) if delta < 0
  next = state.checks + delta
  Ok((Limiter(hits: state.hits, checks: next), next))
end

fn clear_window(state: Limiter, _now: Time) : Limiter
  Limiter(hits: {}, checks: state.checks)
end

api
  actor limiter: RateLimiter
  post "/hit/:key" hit_key
  get "/checks" get_checks
  status RateLimited = 429
  status NegativeDelta = 400
end

fn hit_key(limiter: RateLimiter, key: Text) : Result(Int, AppError)
  return Error(RateLimited) if not limiter.allow(key)
  Ok(limiter.current_checks)
end

fn get_checks(limiter: RateLimiter) : Int
  limiter.current_checks
end

test "actor calls, timers, crash/restart and api integration"
  limiter = Actor.start(RateLimiter)
  assert limiter.allow("ada")
  assert limiter.allow("ada")
  assert not limiter.allow("ada")
  assert limiter.current_checks == 3
  assert limiter.adjust(4) == Ok(7)
  assert limiter.adjust(-1) == Error(NegativeDelta)

  Actor.advance(limiter, 1.minute)
  assert limiter.allow("ada")

  assert ApiTest.send(Main, Api.post("/hit/ada"), limiter).status == 200
  assert ApiTest.send(Main, Api.post("/hit/ada"), limiter).status == 429
  assert ApiTest.send(Main, Api.get("/checks"), limiter).text == "10"

  assert ApiTest.send(Main, Api.post("/hit/ada")).status == 200

  Actor.crash(limiter)
  assert Actor.send(limiter, RateLimiter.current_checks) == Error(ServiceDown)
  Actor.restart(limiter)
  assert Actor.send(limiter, RateLimiter.current_checks) == Ok(10)
end
