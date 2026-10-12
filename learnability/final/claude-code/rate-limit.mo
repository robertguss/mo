## A token-bucket rate limiter with one bucket per client.
##
## Each client has a bucket that holds up to `capacity` tokens and refills at
## `per_second` tokens a second. A request takes a token, or is refused with
## how long to wait until there is one. A client the limiter hasn't seen yet
## starts with a full bucket. Time is passed in: the limiter reads no clock,
## so the same calls always give the same answers.
module RateLimit
expose Limiter, LimitError(..), Decision(..), max_capacity, max_per_second,
  limiter, capacity, per_second, request, available, forget_full

## Tokens are counted in parts, a billion to a token, so that refilling is
## exact: a rate with up to 6 decimal places adds a whole number of parts
## each millisecond.
parts_per_token = 1_000_000_000

## The largest capacity, which keeps a full bucket's parts within an `Int`.
max_capacity = 1_000_000_000

## The fastest rate, in tokens a second.
max_per_second = 1_000_000_000.0

## Every client's bucket, all with the same capacity and rate.
struct Limiter
  capacity: Int               # the most tokens a bucket holds
  per_second: Decimal         # the tokens a bucket gains each second
  parts_per_ms: Int           # the parts a bucket gains each millisecond
  buckets: Map(Text, Bucket)  # by client; a client without one has a full bucket
end

## A client's tokens, counted in parts, as they stood at a time.
struct Bucket
  parts: Int    # from 0 up to the parts of a full bucket
  as_of: Time   # when `parts` was last brought up to date
end

## What a request gets.
enum Decision
  Allowed(remaining: Int)         # the whole tokens the client has left
  Refused(retry_after: Duration)  # how long until the client has a token
end

## Why a limiter can't be made.
enum LimitError
  CapacityOutOfRange(capacity: Int)    # below 1, or above max_capacity
  RateOutOfRange(per_second: Decimal)  # 0 or below, or above max_per_second
  RateTooFine(per_second: Decimal)     # with more than 6 decimal places
end

## A limiter whose buckets hold up to `capacity` tokens and gain `per_second`
## tokens each second.
fn limiter(capacity: Int, per_second: Decimal) : Result(Limiter, LimitError)
  promise "a limiter is made exactly when its capacity and rate are in range, and it keeps them"
    (result is Ok(_)) == (capacity >= 1 and capacity <= max_capacity and
      per_second > 0 and per_second <= max_per_second and
      (per_second * 1_000_000).floor == per_second * 1_000_000)
    result is Ok(made) implies
      made.capacity == capacity and made.per_second == per_second and made.buckets.empty?
    result is Ok(exact) implies
      exact.parts_per_ms.to_decimal * 1000 == per_second * parts_per_token.to_decimal
    allows capacity: 2, per_second: 0.5,
      result: Ok(Limiter(capacity: 2, per_second: 0.5, parts_per_ms: 500_000, buckets: {}))
    allows capacity: 0, per_second: 0.5, result: Error(CapacityOutOfRange(capacity: 0))
    forbids capacity: 0, per_second: 0.5,
      result: Ok(Limiter(capacity: 0, per_second: 0.5, parts_per_ms: 500_000, buckets: {}))
    forbids capacity: 2, per_second: 0.5,
      result: Ok(Limiter(capacity: 3, per_second: 0.5, parts_per_ms: 500_000, buckets: {}))
    forbids capacity: 2, per_second: 0.5,
      result: Ok(Limiter(capacity: 2, per_second: 0.5, parts_per_ms: 500, buckets: {}))
  end
  examples
    limiter(0, 1) == Error(CapacityOutOfRange(capacity: 0))
    limiter(max_capacity + 1, 1) == Error(CapacityOutOfRange(capacity: max_capacity + 1))
    limiter(1, 0) == Error(RateOutOfRange(per_second: 0))
    limiter(1, -2) == Error(RateOutOfRange(per_second: -2))
    limiter(1, 0.0000001) == Error(RateTooFine(per_second: 0.0000001))
    limiter(2, 0.5) == Ok(Limiter(capacity: 2, per_second: 0.5, parts_per_ms: 500_000, buckets: {}))
  end

  return Error(CapacityOutOfRange(capacity: capacity)) if capacity < 1 or capacity > max_capacity
  return Error(RateOutOfRange(per_second: per_second)) if per_second <= 0 or per_second > max_per_second
  # The parts gained each second, spread over its 1000 milliseconds.
  case (per_second * parts_per_token.to_decimal / 1000).to_int
    Some(step):
      Ok(Limiter(capacity: capacity, per_second: per_second, parts_per_ms: step, buckets: {}))
    None: Error(RateTooFine(per_second: per_second))
  end
end

## The most tokens a client's bucket holds.
fn capacity(limiter: Limiter) : Int
  examples
    capacity(one_a_second) == 1
  end

  limiter.capacity
end

## The tokens a client's bucket gains each second.
fn per_second(limiter: Limiter) : Decimal
  examples
    per_second(one_a_second) == 1
  end

  limiter.per_second
end

## Takes a token from the client's bucket at `now`, or says how long until
## there is one. Returns the limiter to use for the next request. A time
## earlier than one the client's bucket has seen adds no tokens.
fn request(limiter: Limiter, client: Text, now: Time) : (Limiter, Decision)
  promise "a request is allowed exactly when its client has a whole token, and then takes just that one"
    (result.1 is Allowed(remaining: _)) == (available(limiter, client, now) >= 1)
    result.1 is Allowed(remaining: left) implies
      left == available(limiter, client, now) - 1 and available(result.0, client, now) == left
    result.1 is Refused(retry_after: _) implies available(result.0, client, now) == 0
    allows limiter: one_a_second, client: "ana", now: noon,
      result: (ana_empty, Allowed(remaining: 0))
    forbids limiter: one_a_second, client: "ana", now: noon,
      result: (one_a_second, Refused(retry_after: 1.seconds))
    forbids limiter: one_a_second, client: "ana", now: noon,
      result: (one_a_second, Allowed(remaining: 0))
  end
  promise "a refusal says how long to wait: by then the client has a token, and a millisecond sooner it has none"
    result.1 is Refused(retry_after: delay) implies delay > 0.seconds
    result.1 is Refused(retry_after: wait) and now.checked_add(wait) is Some(ready) implies
      available(result.0, client, ready) >= 1
    result.1 is Refused(retry_after: pause) and pause > 1.milliseconds and
      now.checked_add(pause - 1.milliseconds) is Some(sooner) implies
      available(result.0, client, sooner) == 0
    allows limiter: ana_empty, client: "ana", now: noon,
      result: (ana_empty, Refused(retry_after: 1.seconds))
    forbids limiter: ana_empty, client: "ana", now: noon,
      result: (ana_empty, Refused(retry_after: 500.milliseconds))
    forbids limiter: ana_empty, client: "ana", now: noon,
      result: (ana_empty, Refused(retry_after: 2.seconds))
  end
  promise "a request changes no other client's bucket, nor the limits"
    result.0.buckets.remove(client) == limiter.buckets.remove(client)
    result.0.capacity == limiter.capacity and result.0.per_second == limiter.per_second
    result.0.parts_per_ms == limiter.parts_per_ms
    allows limiter: one_a_second, client: "ana", now: noon,
      result: (ana_empty, Allowed(remaining: 0))
    forbids limiter: one_a_second, client: "ana", now: noon,
      result: (with_bucket(ana_empty, "bo", Bucket(parts: 0, as_of: noon)), Allowed(remaining: 0))
  end
  examples
    request(one_a_second, "ana", noon) == (ana_empty, Allowed(remaining: 0))
    request(ana_empty, "ana", noon) == (ana_empty, Refused(retry_after: 1.seconds))
    request(ana_empty, "bo", noon) ==
      (with_bucket(ana_empty, "bo", Bucket(parts: 0, as_of: noon)), Allowed(remaining: 0))
  end

  bucket = bucket_at(limiter, client, now)
  if bucket.parts >= parts_per_token
    taken = Bucket(parts: bucket.parts - parts_per_token, as_of: bucket.as_of)
    (with_bucket(limiter, client, taken), Allowed(remaining: taken.parts / parts_per_token))
  else
    (with_bucket(limiter, client, bucket), Refused(retry_after: until_token(bucket, limiter, now)))
  end
end

## How many whole tokens the client has at `now`.
fn available(limiter: Limiter, client: Text, now: Time) : Int
  promise "a client has from none up to the capacity in whole tokens"
    result >= 0
    result <= limiter.capacity
    allows limiter: ana_empty, client: "ana", now: noon, result: 0
    forbids limiter: ana_empty, client: "ana", now: noon, result: 2
    forbids limiter: ana_empty, client: "ana", now: noon, result: -1
  end
  examples
    available(one_a_second, "ana", noon) == 1
    available(ana_empty, "ana", noon + 999.milliseconds) == 0
    available(ana_empty, "ana", noon + 1.seconds) == 1
    available(ana_empty, "bo", noon) == 1
  end

  bucket_at(limiter, client, now).parts / parts_per_token
end

## The limiter without the buckets that are full at `now`. A client without a
## bucket has a full one, so this changes no answer at `now` or later, and it
## keeps the limiter from growing with every client it has ever seen.
fn forget_full(limiter: Limiter, now: Time) : Limiter
  promise "only full buckets are forgotten, so no client's tokens change"
    limiter.buckets.keys.all?(fn(client)
      available(result, client, now) == available(limiter, client, now)
    end)
    result.buckets.keys.all?(fn(client) available(result, client, now) < result.capacity end)
    result.buckets.entries.all?(fn(entry) limiter.buckets.get(entry.0) == Some(entry.1) end)
    result.capacity == limiter.capacity and result.parts_per_ms == limiter.parts_per_ms
    allows limiter: ana_empty, now: noon, result: ana_empty
    allows limiter: ana_empty, now: noon + 1.seconds, result: one_a_second
    forbids limiter: ana_empty, now: noon, result: one_a_second
    forbids limiter: ana_empty, now: noon + 1.seconds, result: ana_empty
  end
  examples
    forget_full(ana_empty, noon + 999.milliseconds) == ana_empty
    forget_full(ana_empty, noon + 1.seconds) == one_a_second
  end

  var kept = limiter
  kept.buckets = limiter.buckets.entries
    .filter(fn(entry) refill(entry.1, limiter, now).parts < full(limiter) end)
    .to_map
  kept
end

## The parts in a full bucket.
fn full(limiter: Limiter) : Int
  limiter.capacity * parts_per_token
end

## The client's bucket, brought up to date at `now`.
fn bucket_at(limiter: Limiter, client: Text, now: Time) : Bucket
  case limiter.buckets.get(client)
    Some(bucket): refill(bucket, limiter, now)
    None: Bucket(parts: full(limiter), as_of: now)
  end
end

## The bucket with the parts it gained since it was last brought up to date.
## It gains them by whole milliseconds, and keeps any part of a millisecond
## left over for next time, so no time is lost however often it is asked. A
## time earlier than the bucket's adds nothing.
fn refill(bucket: Bucket, limiter: Limiter, now: Time) : Bucket
  return bucket if now <= bucket.as_of
  elapsed = (now - bucket.as_of).in_milliseconds
  parts = bucket.parts.saturating_add(elapsed.saturating_mul(limiter.parts_per_ms))
  if parts >= full(limiter)
    Bucket(parts: full(limiter), as_of: now)
  else
    Bucket(parts: parts, as_of: bucket.as_of + elapsed.milliseconds)
  end
end

## How long from `now` until a bucket that holds less than a token holds one:
## the whole milliseconds it needs after its time, less any already passed.
fn until_token(bucket: Bucket, limiter: Limiter, now: Time) : Duration
  missing = parts_per_token - bucket.parts
  step = limiter.parts_per_ms
  steps = (missing + step - 1) / step  # rounded up
  steps.milliseconds - (now - bucket.as_of)
end

## The limiter with the client's bucket replaced.
fn with_bucket(limiter: Limiter, client: Text, bucket: Bucket) : Limiter
  var changed = limiter
  changed.buckets = changed.buckets.set(client, bucket)
  changed
end

## For the examples, promises and tests: a fixed instant.
noon = Time.parse("2026-10-12T12:00:00Z") or stop "the text is a valid RFC 3339 time"

## A limiter of one token a bucket, refilled each second, with no clients yet.
one_a_second = Limiter(capacity: 1, per_second: 1, parts_per_ms: 1_000_000, buckets: {})

## The same limiter once "ana" has taken the one token, at noon.
ana_empty = with_bucket(one_a_second, "ana", Bucket(parts: 0, as_of: noon))

test "a new client starts with a full bucket and can use it all at once"
  assert limiter(3, 1) is Ok(start)
  (one, first) = request(start, "ana", noon)
  (two, second) = request(one, "ana", noon)
  (three, third) = request(two, "ana", noon)
  (_, fourth) = request(three, "ana", noon)
  assert first == Allowed(remaining: 2)
  assert second == Allowed(remaining: 1)
  assert third == Allowed(remaining: 0)
  assert fourth == Refused(retry_after: 1.seconds)
end

test "a refused request says how long until the next token"
  assert limiter(1, 2) is Ok(start)
  (one, first) = request(start, "ana", noon)
  (two, second) = request(one, "ana", noon + 100.milliseconds)
  (_, third) = request(two, "ana", noon + 500.milliseconds)
  assert first == Allowed(remaining: 0)
  assert second == Refused(retry_after: 400.milliseconds)
  assert third == Allowed(remaining: 0)
end

test "the token is there once the wait is over, and not a millisecond before"
  assert limiter(1, 1) is Ok(start)
  (one, _) = request(start, "ana", noon)
  (_, early) = request(one, "ana", noon + 999.milliseconds)
  (_, on_time) = request(one, "ana", noon + 1.seconds)
  assert early == Refused(retry_after: 1.milliseconds)
  assert on_time == Allowed(remaining: 0)
end

test "a bucket refills up to its capacity and no further"
  assert limiter(2, 1) is Ok(start)
  (one, _) = request(start, "ana", noon)
  (two, _) = request(one, "ana", noon)
  assert available(two, "ana", noon) == 0
  assert available(two, "ana", noon + 1.seconds) == 1
  assert available(two, "ana", noon + 2.seconds) == 2
  assert available(two, "ana", noon + 7.days) == 2
end

test "each client has a bucket of its own"
  assert limiter(1, 1) is Ok(start)
  (one, first) = request(start, "ana", noon)
  (two, second) = request(one, "ana", noon)
  (_, third) = request(two, "bo", noon)
  assert first == Allowed(remaining: 0)
  assert second == Refused(retry_after: 1.seconds)
  assert third == Allowed(remaining: 0)
end

test "a rate below one a second spaces the tokens further apart"
  assert limiter(1, 0.25) is Ok(start)
  (one, _) = request(start, "ana", noon)
  (_, second) = request(one, "ana", noon + 1.seconds)
  assert second == Refused(retry_after: 3.seconds)
end

test "requests less than a millisecond apart still refill the bucket"
  assert limiter(1, 1) is Ok(start)
  assert Time.parse("2026-10-12T12:00:00.0006Z") is Some(soon)
  assert Time.parse("2026-10-12T12:00:00.0012Z") is Some(later)
  (one, _) = request(start, "ana", noon)
  (two, second) = request(one, "ana", soon)
  (_, third) = request(two, "ana", later)
  # Either way the token is due a second after noon: no time was lost.
  assert second == Refused(retry_after: (noon + 1.seconds) - soon)
  assert third == Refused(retry_after: (noon + 1.seconds) - later)
end

test "a time earlier than the bucket's adds no tokens"
  assert limiter(1, 1) is Ok(start)
  assert Time.parse("2026-10-12T11:59:50Z") is Some(before)
  (one, _) = request(start, "ana", noon)
  (_, second) = request(one, "ana", before)
  assert second == Refused(retry_after: 11.seconds)
end

test "forgetting full buckets changes no client's tokens"
  assert limiter(2, 1) is Ok(start)
  (one, _) = request(start, "ana", noon)
  (two, _) = request(one, "bo", noon)
  (three, _) = request(two, "bo", noon)
  later = noon + 1.seconds
  kept = forget_full(three, later)
  assert kept.buckets.keys == ["bo"]
  assert available(kept, "ana", later) == 2
  assert available(kept, "bo", later) == 1
  assert request(kept, "ana", later).1 == request(three, "ana", later).1
  assert request(kept, "bo", later).1 == request(three, "bo", later).1
end

test "part of a token stays in the bucket until the rest comes"
  assert limiter(3, 1) is Ok(start)
  (one, _) = request(start, "ana", noon)
  (two, _) = request(one, "ana", noon)
  (three, _) = request(two, "ana", noon)
  later = noon + 2500.milliseconds
  (four, fourth) = request(three, "ana", later)
  (five, fifth) = request(four, "ana", later)
  (_, sixth) = request(five, "ana", later)
  assert fourth == Allowed(remaining: 1)
  assert fifth == Allowed(remaining: 0)
  assert sixth == Refused(retry_after: 500.milliseconds)
end

test "a limiter tells its capacity and rate"
  assert limiter(5, 2.5) is Ok(start)
  assert capacity(start) == 5
  assert per_second(start) == 2.5
end
