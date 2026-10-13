## A token-bucket rate limiter, with one bucket per client. A bucket holds up to
## `capacity` tokens and refills at `per_second` tokens a second; a request
## takes one token, or is refused when there isn't one. The time is passed in,
## so the limiter never reads a clock and its tests never wait.
module RateLimit
expose Limiter, Decision(..), new_limiter, request

struct Bucket
  tokens: Decimal
  updated: Time
end

struct Limiter
  capacity: Decimal
  per_second: Decimal
  buckets: Map(Text, Bucket)
end

enum Decision
  Allowed(remaining: Decimal)
  Refused(retry_after: Duration)
end

## A limiter, or None unless the capacity is from 1 to 1,000,000,000 tokens and
## the rate from 0.001 to 1,000,000 tokens a second.
fn new_limiter(capacity: Decimal, per_second: Decimal) : Option(Limiter)
  examples
    new_limiter(10, 2) is Some(_)
    new_limiter(0.5, 2) == None
    new_limiter(10, 0) == None
  end

  return None if capacity < 1 or capacity > 1_000_000_000
  return None if per_second < 0.001 or per_second > 1_000_000
  Some(Limiter(capacity: capacity, per_second: per_second, buckets: {}))
end

fn request(limiter: Limiter, client: Text, now: Time) : (Limiter, Decision)
  promise "a client is never allowed more tokens than its bucket holds"
    result.1 is Allowed(remaining: left) implies left >= 0 and left <= limiter.capacity - 1
    allows limiter: limiter_of(2), client: "a", now: t0, result: (
      limiter_of(2),
      Allowed(remaining: 1)
    )
    forbids limiter: limiter_of(2), client: "a", now: t0, result: (
      limiter_of(2),
      Allowed(remaining: 2)
    )
  end

  full = Bucket(tokens: limiter.capacity, updated: now)
  bucket = refill(limiter.buckets.get(client) or full, limiter, now)
  (kept, decision) = if bucket.tokens >= 1
    (Bucket(tokens: bucket.tokens - 1, updated: now), Allowed(remaining: bucket.tokens - 1))
  else
    (bucket, Refused(retry_after: wait_for_one(bucket.tokens, limiter.per_second)))
  end
  var after = limiter
  after.buckets = after.buckets.set(client, kept)
  (after, decision)
end

## The bucket with the tokens that dripped in since it was last updated. A clock
## that went backwards adds nothing.
fn refill(bucket: Bucket, limiter: Limiter, now: Time) : Bucket
  elapsed = (now - bucket.updated).in_seconds.max(0)
  tokens = (bucket.tokens + elapsed * limiter.per_second).min(limiter.capacity)
  Bucket(tokens: tokens, updated: now)
end

## How long until a bucket holding `tokens`, less than one, holds one.
fn wait_for_one(tokens: Decimal, per_second: Decimal) : Duration
  wait = ((1 - tokens) * 1000 / per_second).ceil
  case wait.to_int
    Some(n): n.milliseconds
    None: stop "the rate is at least 0.001 a second, so the wait is at most 10^6 milliseconds"
  end
end

t0 = Time.parse("2026-10-12T09:00:00Z") or stop "the time is written correctly"

fn limiter_of(capacity: Decimal) : Limiter
  Limiter(capacity: capacity, per_second: 2, buckets: {})
end

test "a bucket of two allows two requests, then refills in half a second"
  assert new_limiter(2, 2) is Some(limiter)
  (after_one, first) = request(limiter, "ana", t0)
  (after_two, second) = request(after_one, "ana", t0)
  (after_three, third) = request(after_two, "ana", t0)
  assert first == Allowed(remaining: 1)
  assert second == Allowed(remaining: 0)
  assert third == Refused(retry_after: 500.milliseconds)
  (_, later) = request(after_three, "ana", t0 + 500.milliseconds)
  assert later == Allowed(remaining: 0)
end

test "each client has its own bucket"
  assert new_limiter(1, 1) is Some(limiter)
  (after_ana, _) = request(limiter, "ana", t0)
  (_, for_ben) = request(after_ana, "ben", t0)
  assert for_ben == Allowed(remaining: 0)
end
