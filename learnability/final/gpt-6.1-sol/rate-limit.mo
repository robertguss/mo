## Per-client token buckets. Callers keep the state returned by request.
## New clients start full; each accepted request consumes exactly one token.
module Answers.RateLimit
expose RateLimit, Decision(..), RateLimitError(..), create, request

struct RateLimit
  capacity: Int
  rate_units: BigInt
  buckets: Map(Text, Bucket)
end

struct Bucket
  credit: BigInt
  updated: Time
end

enum Decision
  Accepted
  Refused(wait_microseconds: BigInt)
end

enum RateLimitError
  InvalidCapacity
  InvalidRate
  TimeWentBackwards
end

# Credit is measured in 10^-24 tokens. A rate unit is 10^-18 tokens
# per second, so one rate unit adds one credit unit per microsecond.
token_units = 1.to_big * 10.to_big.pow(24)
epoch = Time.parse("2026-01-01T00:00:00Z") or stop "the fixed example time is valid RFC 3339"

## Construct a limiter with a positive capacity and positive tokens per second.
fn create(capacity: Int, tokens_per_second: Decimal) : Result(RateLimit, RateLimitError)
  promise "only positive capacity and rate produce an empty configured limiter"
    capacity < 1 implies result == Error(InvalidCapacity)
    capacity >= 1 and tokens_per_second <= 0 implies result == Error(InvalidRate)
    capacity >= 1 and tokens_per_second > 0 implies
      result is Ok(configured) and configured.capacity == capacity and
      configured.rate_units == scaled_rate(tokens_per_second) and
      configured.rate_units > 0.to_big and configured.buckets.empty?
    allows capacity: 2, tokens_per_second: 0.5,
      result: Ok(RateLimit(capacity: 2, rate_units: 500_000_000_000_000_000.to_big, buckets: {}))
    forbids capacity: 0, tokens_per_second: 1,
      result: Ok(RateLimit(capacity: 0, rate_units: 1_000_000_000_000_000_000.to_big, buckets: {}))
  end
  examples
    create(0, 1) == Error(InvalidCapacity)
    create(1, 0) == Error(InvalidRate)
    create(1, -0.1) == Error(InvalidRate)
  end

  return Error(InvalidCapacity) if capacity < 1
  return Error(InvalidRate) if tokens_per_second <= 0
  Ok(RateLimit(
    capacity: capacity,
    rate_units: scaled_rate(tokens_per_second),
    buckets: {}
  ))
end

# Decimal's fixed text gives all 18 fractional digits without losing precision.
fn scaled_rate(rate: Decimal) : BigInt
  var units = 0.to_big
  for digit in rate.to_fixed(18).chars
    if digit.ascii_digit?
      units = units * 10.to_big + (digit.code - '0'.code).to_big
    end
  end
  if rate < 0: -units else: units
end

fn elapsed_microseconds(later: Time, earlier: Time) : BigInt
  case ((later - earlier).in_seconds * 1_000_000).to_int
    Some(value): value.to_big
    None: stop "a difference of two supported Times fits in Int microseconds"
  end
end

## Time is supplied by the caller, not read from a Clock.
## Backward time is rejected per client; other clients have independent clocks.
## A refusal updates refill credit but does not consume or reserve a token.
## Its wait assumes no other request for this client consumes tokens first.
fn request(limiter: RateLimit, client: Text, now: Time) : Result((RateLimit, Decision), RateLimitError)
  promise "one step preserves other clients, caps refill, and consumes exactly one token or gives the least wait"
    case result
      Error(InvalidCapacity) | Error(InvalidRate): false
      Error(TimeWentBackwards):
        limiter.buckets.get(client) is Some(old) and now < old.updated
      Ok((next, verdict)):
        maximum = limiter.capacity.to_big * token_units
        old = limiter.buckets.get(client) or Bucket(credit: maximum, updated: now)
        available = (old.credit + elapsed_microseconds(now, old.updated) * limiter.rate_units).min(maximum)
        now >= old.updated and next.capacity == limiter.capacity and
          next.rate_units == limiter.rate_units and
          next.buckets.remove(client) == limiter.buckets.remove(client) and
          next.buckets.get(client) is Some(bucket) and bucket.updated == now and
          bucket.credit >= 0.to_big and bucket.credit <= maximum and
          case verdict
            Accepted:
              available >= token_units and bucket.credit == available - token_units
            Refused(wait_microseconds: wait):
              available < token_units and bucket.credit == available and wait > 0.to_big and
                wait * limiter.rate_units >= token_units - available and
                (wait - 1.to_big) * limiter.rate_units < token_units - available
          end
    end
    allows limiter: RateLimit(capacity: 1, rate_units: 1_000_000_000_000_000_000.to_big, buckets: {}),
      client: "a", now: epoch,
      result: Ok((RateLimit(capacity: 1, rate_units: 1_000_000_000_000_000_000.to_big,
        buckets: {"a": Bucket(credit: 0.to_big, updated: epoch)}), Accepted))
    forbids limiter: RateLimit(capacity: 1, rate_units: 1_000_000_000_000_000_000.to_big, buckets: {}),
      client: "a", now: epoch,
      result: Ok((RateLimit(capacity: 1, rate_units: 1_000_000_000_000_000_000.to_big,
        buckets: {"a": Bucket(credit: token_units, updated: epoch)}), Accepted))
  end

  full = limiter.capacity.to_big * token_units
  previous = limiter.buckets.get(client) or Bucket(credit: full, updated: now)
  return Error(TimeWentBackwards) if now < previous.updated

  micros = elapsed_microseconds(now, previous.updated)
  refilled = (previous.credit + micros * limiter.rate_units).min(full)
  allowed? = refilled >= token_units
  remaining = if allowed?: refilled - token_units else: refilled
  decision = if allowed?
    Accepted
  else
    deficit = token_units - refilled
    wait = (deficit + limiter.rate_units - 1.to_big) / limiter.rate_units
    Refused(wait_microseconds: wait)
  end
  var updated = limiter
  updated.buckets = updated.buckets.set(client, Bucket(credit: remaining, updated: now))
  Ok((updated, decision))
end

test "nonpositive capacity and rate are rejected"
  assert create(-1, 1) == Error(InvalidCapacity)
  assert create(0, 0) == Error(InvalidCapacity)
  assert create(2, 0) == Error(InvalidRate)
  assert create(2, -0.5) == Error(InvalidRate)
end

test "clients have separate bursts and old states remain unchanged"
  assert create(3, 2) is Ok(initial)
  var current = initial
  for _ in 0..3
    assert request(current, "alice", epoch) is Ok(step)
    assert step.1 == Accepted
    current = step.0
  end
  assert request(current, "alice", epoch) is Ok(denied)
  assert denied.1 == Refused(wait_microseconds: 500_000.to_big)
  assert request(denied.0, "bob", epoch) is Ok(other)
  assert other.1 == Accepted
  assert request(other.0, "alice", epoch) is Ok(still_denied)
  assert still_denied.1 == Refused(wait_microseconds: 500_000.to_big)
  assert request(initial, "alice", epoch) is Ok(replayed)
  assert replayed.1 == Accepted
end

test "refusals preserve partial tokens and do not restart the refill delay"
  assert create(1, 0.5) is Ok(initial)
  assert request(initial, "alice", epoch) is Ok(first)
  assert first.1 == Accepted
  assert request(first.0, "alice", epoch + 500.milliseconds) is Ok(partial)
  assert partial.1 == Refused(wait_microseconds: 1_500_000.to_big)
  assert request(partial.0, "alice", epoch + 1.seconds) is Ok(half)
  assert half.1 == Refused(wait_microseconds: 1_000_000.to_big)
  assert request(half.0, "alice", epoch + 2.seconds) is Ok(ready)
  assert ready.1 == Accepted
  assert request(ready.0, "alice", epoch + 2.seconds) is Ok(empty)
  assert empty.1 == Refused(wait_microseconds: 2_000_000.to_big)
end

test "retry delays round up and the adjacent microseconds straddle acceptance"
  before = Time.parse("2026-01-01T00:00:00.333333Z") or stop "the test time is valid"
  after = Time.parse("2026-01-01T00:00:00.333334Z") or stop "the test time is valid"
  assert create(1, 3) is Ok(initial)
  assert request(initial, "alice", epoch) is Ok(first)
  assert first.1 == Accepted
  assert request(first.0, "alice", epoch) is Ok(waiting)
  assert waiting.1 == Refused(wait_microseconds: 333_334.to_big)
  assert request(waiting.0, "alice", before) is Ok(early)
  assert early.1 == Refused(wait_microseconds: 1.to_big)
  assert request(early.0, "alice", after) is Ok(ready)
  assert ready.1 == Accepted
  assert request(ready.0, "alice", after) is Ok(residual)
  assert residual.1 == Refused(wait_microseconds: 333_333.to_big)
end

test "idle refill cannot bank tokens beyond capacity"
  assert create(2, 1) is Ok(initial)
  assert request(initial, "alice", epoch) is Ok(first)
  assert first.1 == Accepted
  assert request(first.0, "alice", epoch + 10.seconds) is Ok(second)
  assert second.1 == Accepted
  assert request(second.0, "alice", epoch + 10.seconds) is Ok(third)
  assert third.1 == Accepted
  assert request(third.0, "alice", epoch + 10.seconds) is Ok(denied)
  assert denied.1 == Refused(wait_microseconds: 1_000_000.to_big)
end

test "backward timestamps are errors only for the affected client"
  assert create(1, 1) is Ok(initial)
  assert request(initial, "alice", epoch + 5.seconds) is Ok(first)
  assert first.1 == Accepted
  assert request(first.0, "alice", epoch) == Error(TimeWentBackwards)
  assert request(first.0, "bob", epoch) is Ok(other)
  assert other.1 == Accepted
  assert request(other.0, "alice", epoch + 5.seconds) is Ok(same_time)
  assert same_time.1 == Refused(wait_microseconds: 1_000_000.to_big)
  assert request(same_time.0, "alice", epoch + 6.seconds) is Ok(ready)
  assert ready.1 == Accepted
end

test "the smallest positive decimal rate has an exact unbounded retry delay"
  assert create(1, 0.000000000000000001) is Ok(initial)
  assert request(initial, "alice", epoch) is Ok(first)
  assert first.1 == Accepted
  assert request(first.0, "alice", epoch) is Ok(denied)
  assert denied.1 == Refused(wait_microseconds: 10.to_big.pow(24))
  assert request(denied.0, "alice", epoch + 1.seconds) is Ok(partial)
  assert partial.1 == Refused(wait_microseconds: 10.to_big.pow(24) - 1_000_000.to_big)
end

test "large rates and the full Time range refill without fixed-width overflow"
  start = Time.parse("0001-01-01T00:00:00Z") or stop "the first supported year is valid"
  finish = Time.parse("9999-12-31T23:59:59.999999Z") or stop "the last supported year is valid"
  assert create(1, 100000000000000000000.0) is Ok(initial)
  assert request(initial, "alice", start) is Ok(first)
  assert first.1 == Accepted
  assert request(first.0, "alice", start) is Ok(denied)
  assert denied.1 == Refused(wait_microseconds: 1.to_big)
  assert request(denied.0, "alice", finish) is Ok(refilled)
  assert refilled.1 == Accepted
  assert request(refilled.0, "alice", finish) is Ok(capped)
  assert capped.1 == Refused(wait_microseconds: 1.to_big)
end
