## Token buckets keyed by client, with time supplied by the caller.
## New clients start full. Floating-point tokens allow fractional refills.
module RateLimit
expose Limiter, Decision(..), LimitError(..), create, request

struct Bucket
  tokens: Float
  updated: Time
end

struct Limiter
  capacity: Int
  rate: Float
  buckets: Map(Text, Bucket)
end

enum Decision
  Accepted
  Refused(wait_seconds: Float)
end

enum LimitError
  InvalidConfiguration
  TimeWentBackwards
end

epoch = Time.parse("2026-01-01T00:00:00Z") or stop "the epoch is valid RFC 3339"

fn bounded?(limiter: Limiter) : Bool
  limiter.buckets.values.all?(fn(bucket)
    bucket.tokens >= 0 and bucket.tokens <= limiter.capacity.to_float
  end)
end

fn create(capacity: Int, rate: Float) : Result(Limiter, LimitError)
  promise "valid configuration makes an empty limiter with bounded buckets"
    case result
      Ok(limiter): capacity > 0 and capacity <= 9_007_199_254_740_992 and rate > 0 and
        not rate.infinite? and not rate.nan? and limiter.capacity == capacity and
        limiter.rate == rate and limiter.buckets.empty? and bounded?(limiter)
      Error(InvalidConfiguration): capacity <= 0 or capacity > 9_007_199_254_740_992 or
        rate <= 0 or rate.infinite? or rate.nan?
      Error(TimeWentBackwards): false
    end
    allows capacity: 2, rate: 1.0, result: Ok(Limiter(capacity: 2, rate: 1.0, buckets: {}))
    forbids capacity: 2, rate: 1.0, result: Ok(Limiter(capacity: 3, rate: 1.0, buckets: {}))
  end

  # Larger capacities cannot represent every whole token in a Float.
  return Error(InvalidConfiguration) if capacity <= 0 or capacity > 9_007_199_254_740_992 or
    rate <= 0 or rate.nan? or rate.infinite?
  Ok(Limiter(capacity: capacity, rate: rate, buckets: {}))
end

fn request(limiter: Limiter, client: Text, now: Time) : Result((Limiter, Decision), LimitError)
  promise "requests keep tokens bounded, affect only their client, and refusals give a positive wait"
    case result
      Ok((updated, decision)): bounded?(updated) and updated.capacity == limiter.capacity and
        updated.rate == limiter.rate and updated.buckets.has?(client) and
        limiter.buckets.entries.all?(fn(entry)
          entry.0 == client or updated.buckets.get(entry.0) == Some(entry.1)
        end) and
        case decision
          Accepted: true
          Refused(wait_seconds: delay): delay > 0
        end
      Error(TimeWentBackwards): limiter.buckets.get(client) is Some(bucket) and now < bucket.updated
      Error(InvalidConfiguration): false
    end
    allows limiter: Limiter(capacity: 2, rate: 1.0, buckets: {}), client: "a", now: epoch,
      result: Ok((Limiter(capacity: 2, rate: 1.0, buckets: {"a": Bucket(tokens: 1.0, updated: epoch)}), Accepted))
    forbids limiter: Limiter(capacity: 2, rate: 1.0, buckets: {}), client: "a", now: epoch,
      result: Ok((Limiter(capacity: 2, rate: 1.0, buckets: {"a": Bucket(tokens: 3.0, updated: epoch)}), Accepted))
  end

  bucket = limiter.buckets.get(client) or Bucket(tokens: limiter.capacity.to_float, updated: now)
  return Error(TimeWentBackwards) if now < bucket.updated
  available = bucket.tokens + (now - bucket.updated).in_seconds.to_float * limiter.rate
  capped = if available >= limiter.capacity.to_float: limiter.capacity.to_float else: available
  decision = if capped >= 1
    Accepted
  else
    calculated = (1 - capped) / limiter.rate
    # Time advances in whole microseconds, even when a Float wait underflows.
    Refused(wait_seconds: if calculated >= 0.000001: calculated else: 0.000001)
  end
  remaining = if decision is Accepted: capped - 1 else: capped
  var updated = limiter
  updated.buckets = updated.buckets.set(client, Bucket(tokens: remaining, updated: now))
  Ok((updated, decision))
end

test "clients are independent and a partial refill gives the remaining wait"
  assert create(2, 2.0) is Ok(initial)
  assert request(initial, "a", epoch) is Ok((first, Accepted))
  assert request(first, "a", epoch) is Ok((second, Accepted))
  assert request(second, "a", epoch) is Ok((third, Refused(wait_seconds: delay)))
  assert delay == 0.5
  assert request(third, "b", epoch) is Ok((fourth, Accepted))
  assert request(fourth, "a", epoch + 250.milliseconds) is Ok((fifth, Refused(wait_seconds: partial)))
  assert partial == 0.25
  assert request(fifth, "a", epoch + 500.milliseconds) is Ok((sixth, Accepted))
  assert request(sixth, "a", epoch) == Error(TimeWentBackwards)
end

test "refills stop at capacity and invalid configurations are rejected"
  assert create(1, 1.0) is Ok(initial)
  assert request(initial, "a", epoch) is Ok((first, Accepted))
  assert request(first, "a", epoch + 10.seconds) is Ok((second, Accepted))
  assert request(second, "a", epoch + 10.seconds) is Ok((_, Refused(wait_seconds: delay)))
  assert delay == 1.0
  assert create(0, 1.0) == Error(InvalidConfiguration)
  assert create(1, 0.0) == Error(InvalidConfiguration)
end
