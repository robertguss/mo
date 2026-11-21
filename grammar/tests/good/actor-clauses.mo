module Main

actor RateLimiter
  state Map(Text, Int) = {}
  calls allow, remaining, reset
  every 1.minute cleanup
  promise "counts stay nonnegative"
    after.values.all?(fn(c) c >= 0 end)
  end
end

api
  port 8080
  actor limiter: RateLimiter
  get "/check" check_limit
end

fn allow(s: Map(Text, Int), _now: Time, key: Text, limit: Int) : (Map(Text, Int), Bool)
  calls = (s.get(key) or 0) + 1
  return (s, false) if calls > limit
  (s.set(key, calls), true)
end

fn remaining(s: Map(Text, Int), _now: Time, key: Text, limit: Int) : Int
  calls = s.get(key) or 0
  if calls >= limit: 0 else: limit - calls
end

fn reset(s: Map(Text, Int), _now: Time) : (Map(Text, Int), Int)
  ({}, s.size)
end

fn cleanup(_s: Map(Text, Int), _now: Time) : Map(Text, Int)
  {}
end

fn check_limit(limiter: RateLimiter) : Bool
  limiter.allow("ip", 10)
end
