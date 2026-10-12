## A token-bucket rate limiter with one bucket per client.
##
## Each client has a bucket that holds up to `capacity` tokens and refills
## continuously at `per_second` tokens a second. A request takes one token, or,
## when the bucket holds less than a whole token, is refused with how long to
## wait until it will hold one. A client not seen before starts with a full
## bucket.
##
## Nothing here reads a clock: every call is given the time, so a limiter is a
## plain value and the same calls always give the same answers. A server passes
## `clock.now` from its `Clock`, and keeps the limiter each request returns:
##
##   (decision, next) = RateLimit.request(limits, client, clock.now)
module RateLimit
expose Limit, LimitError(..), Limiter, Decision(..), max_capacity, max_per_second,
  limit, limiter, request, available, prune

## The most tokens a bucket may hold.
max_capacity = 1_000_000_000

## The fastest refill a limit may have, in tokens a second.
max_per_second = 1_000_000

## How many requests a client may make: a burst of up to `capacity` at once,
## and `per_second` a second over time. Only `limit` makes one, so every limit
## is in range.
struct Limit
  capacity: Int
  per_second: Decimal
end

## Why `limit` refused to make a limit.
enum LimitError
  BadCapacity(capacity: Int)      # below 1, or above max_capacity
  BadRate(per_second: Decimal)    # not above 0, above max_per_second, or finer than a millionth
end

## A client's bucket: the tokens it held at the instant `updated`.
struct Bucket
  tokens: Decimal
  updated: Time
end

## The buckets of the clients seen and not yet forgotten, under one limit.
struct Limiter
  limit: Limit
  buckets: Map(Text, Bucket)
end

## What a request gets.
enum Decision
  Allowed(remaining: Int)           # it took a token, and this many whole tokens are left
  Refused(retry_after: Duration)    # no whole token now: one will be there after this long
end

## A limit of `capacity` tokens a bucket, refilled at `per_second` tokens a
## second. The capacity is from 1 to max_capacity. The rate is above 0, at most
## max_per_second, and given to at most 6 decimal places, which keeps every
## count of tokens exact.
fn limit(capacity: Int, per_second: Decimal) : Result(Limit, LimitError)
  promise "a limit is made exactly when both numbers are in range, keeps them, and a refusal names the one that isn't"
    (result is Ok(_)) == (capacity >= 1 and capacity <= max_capacity and per_second > 0 and
      per_second <= max_per_second.to_decimal and per_second.round(6) == per_second)
    result is Ok(made) implies made.capacity == capacity and made.per_second == per_second
    result is Error(BadCapacity(capacity: named_capacity)) implies named_capacity == capacity
    result is Error(BadRate(per_second: named_rate)) implies named_rate == per_second
    allows capacity: 10, per_second: 2.5, result: Ok(Limit(capacity: 10, per_second: 2.5))
    allows capacity: 0, per_second: 2.5, result: Error(BadCapacity(capacity: 0))
    forbids capacity: 10, per_second: 2.5, result: Error(BadRate(per_second: 2.5))
    forbids capacity: 10, per_second: 0.0000001, result: Ok(Limit(capacity: 10, per_second: 0.0000001))
    forbids capacity: 10, per_second: 2.5, result: Ok(Limit(capacity: 10, per_second: 5.0))
    forbids capacity: 0, per_second: 2.5, result: Error(BadCapacity(capacity: 1))
  end
  examples
    limit(10, 2.5) == Ok(Limit(capacity: 10, per_second: 2.5))
    limit(0, 2.5) == Error(BadCapacity(capacity: 0))
    limit(10, 0.0) == Error(BadRate(per_second: 0.0))
    limit(10, 0.1234567) == Error(BadRate(per_second: 0.1234567))
  end

  return Error(BadCapacity(capacity: capacity)) if capacity < 1 or capacity > max_capacity
  return Error(BadRate(per_second: per_second)) if per_second <= 0 or
    per_second > max_per_second.to_decimal or per_second.round(6) != per_second
  Ok(Limit(capacity: capacity, per_second: per_second))
end

## A limiter under `limit` that has seen no clients yet.
fn limiter(limit: Limit) : Limiter
  promise "a new limiter works under the limit it is given and has seen no clients"
    result.limit == limit
    result.buckets.empty?
    allows limit: sample_limit, result: Limiter(limit: sample_limit, buckets: {})
    forbids limit: sample_limit, result: sample(0.5)
  end

  Limiter(limit: limit, buckets: {})
end

## Takes a token from `client`'s bucket at `now`, or refuses with how long to
## wait. Returns the decision and the limiter to use next; a refusal returns
## the limiter unchanged.
fn request(limiter: Limiter, client: Text, now: Time) : (Decision, Limiter)
  promise "a request is allowed exactly when the client holds a whole token, and takes just that one"
    (result.0 is Allowed(remaining: _)) == (available(limiter, client, now) >= 1)
    result.0 is Allowed(remaining: left) implies
      available(result.1, client, now) == available(limiter, client, now) - 1 and
      left == whole(available(result.1, client, now))
    allows limiter: sample(1.5), client: "ana", now: epoch, result: (Allowed(remaining: 0), sample(0.5))
    forbids limiter: sample(0.5), client: "ana", now: epoch, result: (Allowed(remaining: 0), sample(-0.5))
    forbids limiter: sample(1.5), client: "ana", now: epoch, result: (Allowed(remaining: 1), sample(1.5))
    forbids limiter: sample(1.5), client: "ana", now: epoch, result: (Allowed(remaining: 1), sample(0.5))
  end
  promise "a refusal changes nothing, and after its wait the client holds a whole token, where a millisecond less is too soon"
    result.0 is Refused(retry_after: _) implies result.1 == limiter
    result.0 is Refused(retry_after: delay) implies
      (now.checked_add(delay) is Some(later) implies available(limiter, client, later) >= 1) and
      (now.checked_add(delay - 1.milliseconds) is Some(sooner) implies available(limiter, client, sooner) < 1)
    allows limiter: sample(0.25), client: "ana", now: epoch,
      result: (Refused(retry_after: 750.milliseconds), sample(0.25))
    forbids limiter: sample(0.25), client: "ana", now: epoch,
      result: (Refused(retry_after: 700.milliseconds), sample(0.25))
    forbids limiter: sample(0.25), client: "ana", now: epoch,
      result: (Refused(retry_after: 800.milliseconds), sample(0.25))
    forbids limiter: sample(0.25), client: "ana", now: epoch,
      result: (Refused(retry_after: 750.milliseconds), sample(1.25))
  end
  promise "a request changes no other client's bucket, and not the limit"
    result.1.limit == limiter.limit
    result.1.buckets.remove(client) == limiter.buckets.remove(client)
    allows limiter: sample(1.5), client: "ana", now: epoch, result: (Allowed(remaining: 0), sample(0.5))
    forbids limiter: sample(1.5), client: "bo", now: epoch, result: (Allowed(remaining: 1), sample(0.5))
  end

  bucket = bucket_at(limiter, client, now)
  return (Refused(retry_after: until_token(bucket, limiter.limit, now)), limiter) if bucket.tokens < 1
  taken = Bucket(tokens: bucket.tokens - 1, updated: bucket.updated)
  after = Limiter(limit: limiter.limit, buckets: limiter.buckets.set(client, taken))
  (Allowed(remaining: whole(taken.tokens)), after)
end

## How many tokens `client` holds at `now`, as an exact fraction.
fn available(limiter: Limiter, client: Text, now: Time) : Decimal
  promise "a client holds from no tokens up to a full bucket"
    result >= 0
    result <= limiter.limit.capacity.to_decimal
    allows limiter: sample(0.5), client: "ana", now: epoch, result: 0.5
    forbids limiter: sample(0.5), client: "ana", now: epoch, result: 2.5
    forbids limiter: sample(0.5), client: "ana", now: epoch, result: -0.5
  end
  promise "a second later a client has lost no tokens and gained at most a second's refill"
    now.checked_add(1.seconds) is Some(later) implies
      available(limiter, client, later) >= result and
      available(limiter, client, later) <= result + limiter.limit.per_second
    allows limiter: sample(0.5), client: "ana", now: epoch, result: 0.5
    forbids limiter: sample(0.5), client: "ana", now: epoch, result: 0.25
    forbids limiter: sample(0.5), client: "ana", now: epoch, result: 2.0
  end
  examples
    available(sample(0.5), "ana", epoch) == 0.5
    available(sample(0.5), "ana", at(1_250)) == 1.75
    available(sample(0.5), "ana", at(60_000)) == 2.0
    available(sample(0.5), "bo", epoch) == 2.0
  end

  bucket_at(limiter, client, now).tokens
end

## Forgets the clients whose buckets are full at `now`. A client with no bucket
## gets a full one, so this changes no answer to a request made at `now` or
## later, and it keeps the limiter only as large as the set of clients still
## using it. Call it from time to time.
fn prune(limiter: Limiter, now: Time) : Limiter
  promise "forgetting changes no client's tokens at now, and keeps only the buckets short of full"
    result.limit == limiter.limit
    limiter.buckets.keys.all?(fn(client) available(result, client, now) == available(limiter, client, now) end)
    result.buckets.keys.all?(fn(client)
      limiter.buckets.has?(client) and available(result, client, now) < result.limit.capacity.to_decimal
    end)
    allows limiter: sample(0.5), now: epoch, result: sample(0.5)
    allows limiter: sample(0.5), now: at(2_000), result: Limiter(limit: sample_limit, buckets: {})
    forbids limiter: sample(0.5), now: epoch, result: Limiter(limit: sample_limit, buckets: {})
    forbids limiter: sample(0.5), now: at(2_000), result: sample(0.5)
  end

  full = limiter.limit.capacity.to_decimal
  kept = limiter.buckets.entries.filter(fn(entry) fill(entry.1, limiter.limit, now).tokens < full end)
  Limiter(limit: limiter.limit, buckets: kept.to_map)
end

## `client`'s bucket brought up to `now`. A client not seen yet, or forgotten,
## has a full bucket.
fn bucket_at(limiter: Limiter, client: Text, now: Time) : Bucket
  case limiter.buckets.get(client)
    Some(bucket): fill(bucket, limiter.limit, now)
    None: Bucket(tokens: limiter.limit.capacity.to_decimal, updated: now)
  end
end

## `bucket` refilled for the time from its update to `now`, up to the capacity.
## Time before its update adds nothing, so a request stamped earlier than the
## last one never mints tokens, and the update time never goes back.
fn fill(bucket: Bucket, limit: Limit, now: Time) : Bucket
  examples
    fill(Bucket(tokens: 0.5, updated: epoch), sample_limit, at(1_000)) == Bucket(tokens: 1.5, updated: at(1_000))
    fill(Bucket(tokens: 0.5, updated: epoch), sample_limit, at(60_000)) == Bucket(tokens: 2.0, updated: at(60_000))
    fill(Bucket(tokens: 0.5, updated: at(1_000)), sample_limit, epoch) == Bucket(tokens: 0.5, updated: at(1_000))
  end

  return bucket if now <= bucket.updated
  gained = (now - bucket.updated).in_seconds * limit.per_second
  Bucket(tokens: (bucket.tokens + gained).min(limit.capacity.to_decimal), updated: now)
end

## How long from `now` until `bucket`, which holds less than a token, holds a
## whole one: any time still to come before its update, then the refill,
## rounded up to the millisecond so that waiting that long is always enough.
fn until_token(bucket: Bucket, limit: Limit, now: Time) : Duration
  examples
    until_token(Bucket(tokens: 0.25, updated: epoch), sample_limit, epoch) == 750.milliseconds
    until_token(Bucket(tokens: 0.25, updated: at(2_000)), sample_limit, epoch) == 2_750.milliseconds
    until_token(Bucket(tokens: 0.0, updated: epoch), Limit(capacity: 1, per_second: 3.0), epoch) == 334.milliseconds
  end

  millis = ((1 - bucket.tokens) * 1000 / limit.per_second).ceil
  case millis.to_int
    Some(ms): (bucket.updated - now) + ms.milliseconds
    None: stop "less than a token at the slowest rate is at most a billion milliseconds"
  end
end

## The whole tokens in `amount`, rounded down.
fn whole(amount: Decimal) : Int
  examples
    whole(1.75) == 1
    whole(0.5) == 0
    whole(2.0) == 2
  end

  case amount.floor.to_int
    Some(n): n
    None: stop "a bucket holds at most max_capacity tokens, which fits an Int"
  end
end

# What the anchors, examples and tests build on.

## The instant the anchors, examples and tests count from.
epoch = case Time.parse("2026-01-01T00:00:00Z")
  Some(time): time
  None: stop "the epoch is valid RFC 3339 text"
end

## The instant `ms` milliseconds after `epoch`.
fn at(ms: Int) : Time
  epoch + ms.milliseconds
end

## The limit the anchors and examples use: 2 tokens, refilled at 1 a second.
sample_limit = Limit(capacity: 2, per_second: 1.0)

## A limiter under `sample_limit` in which "ana" held `tokens` at `epoch`.
fn sample(tokens: Decimal) : Limiter
  Limiter(limit: sample_limit, buckets: {"ana": Bucket(tokens: tokens, updated: epoch)})
end

## A new limiter under a limit the tests know is in range.
fn fresh(capacity: Int, per_second: Decimal) : Limiter
  case limit(capacity, per_second)
    Ok(made): limiter(made)
    Error(_): stop "the tests ask only for limits in range"
  end
end

## The decisions for `requests`, each made on the limiter the one before it
## left, and the limiter the last one leaves.
fn replay(start: Limiter, requests: List((Text, Time))) : (List(Decision), Limiter)
  var state = start
  var decisions = []
  for (client, time) in requests
    (decision, next) = request(state, client, time)
    decisions = decisions.append(decision)
    state = next
  end
  (decisions, state)
end

test "a new client starts with a full bucket, and is refused once it is empty"
  (decisions, _) = replay(fresh(3, 1.0), [("ana", at(0)), ("ana", at(0)), ("ana", at(0)), ("ana", at(0))])
  assert decisions == [
    Allowed(remaining: 2),
    Allowed(remaining: 1),
    Allowed(remaining: 0),
    Refused(retry_after: 1.seconds)
  ]
end

test "waiting as long as a refusal says is enough, and a millisecond less is not"
  (decisions, _) = replay(fresh(2, 1.0), [
    ("ana", at(0)),
    ("ana", at(0)),
    ("ana", at(0)),
    ("ana", at(999)),
    ("ana", at(1_000))
  ])
  assert decisions == [
    Allowed(remaining: 1),
    Allowed(remaining: 0),
    Refused(retry_after: 1.seconds),
    Refused(retry_after: 1.milliseconds),
    Allowed(remaining: 0)
  ]
end

test "tokens come back at the rate, but never past the capacity"
  (_, emptied) = replay(fresh(2, 0.5), [("ana", at(0)), ("ana", at(0))])
  assert available(emptied, "ana", at(0)) == 0
  assert available(emptied, "ana", at(1_000)) == 0.5
  assert available(emptied, "ana", at(3_000)) == 1.5
  assert available(emptied, "ana", at(4_000)) == 2
  assert available(emptied, "ana", at(3_600_000)) == 2
end

test "a request takes a whole token and the fraction left over carries on"
  (decisions, _) = replay(fresh(3, 1.0), [
    ("ana", at(0)),
    ("ana", at(0)),
    ("ana", at(0)),
    ("ana", at(1_500)),
    ("ana", at(1_500)),
    ("ana", at(2_000))
  ])
  assert decisions == [
    Allowed(remaining: 2),
    Allowed(remaining: 1),
    Allowed(remaining: 0),
    Allowed(remaining: 0),
    Refused(retry_after: 500.milliseconds),
    Allowed(remaining: 0)
  ]
end

test "each client has a bucket of its own"
  (decisions, _) = replay(fresh(1, 1.0), [("ana", at(0)), ("ana", at(0)), ("bo", at(0)), ("bo", at(500))])
  assert decisions == [
    Allowed(remaining: 0),
    Refused(retry_after: 1.seconds),
    Allowed(remaining: 0),
    Refused(retry_after: 500.milliseconds)
  ]
end

test "a rate below one a second makes a client wait more than a second"
  (decisions, _) = replay(fresh(1, 0.25), [("ana", at(0)), ("ana", at(1_000)), ("ana", at(4_000))])
  assert decisions == [
    Allowed(remaining: 0),
    Refused(retry_after: 3.seconds),
    Allowed(remaining: 0)
  ]
end

test "a wait is rounded up to the next millisecond"
  (decisions, _) = replay(fresh(1, 3.0), [("ana", at(0)), ("ana", at(0)), ("ana", at(333)), ("ana", at(334))])
  assert decisions == [
    Allowed(remaining: 0),
    Refused(retry_after: 334.milliseconds),
    Refused(retry_after: 1.milliseconds),
    Allowed(remaining: 0)
  ]
end

test "a request stamped before the last one gets no tokens for the time in between"
  (decisions, _) = replay(fresh(1, 1.0), [("ana", at(10_000)), ("ana", at(5_000)), ("ana", at(11_000))])
  assert decisions == [
    Allowed(remaining: 0),
    Refused(retry_after: 6.seconds),
    Allowed(remaining: 0)
  ]
end

test "a refused request leaves the limiter as it was"
  (first, emptied) = request(fresh(1, 1.0), "ana", at(0))
  (second, unchanged) = request(emptied, "ana", at(500))
  assert first == Allowed(remaining: 0)
  assert second == Refused(retry_after: 500.milliseconds)
  assert unchanged == emptied
end

test "pruning forgets only the clients whose buckets are full again"
  (_, busy) = replay(fresh(2, 1.0), [("ana", at(0)), ("bo", at(0)), ("bo", at(0))])
  pruned = prune(busy, at(1_000))
  assert busy.buckets.keys == ["ana", "bo"]
  assert pruned.buckets.keys == ["bo"]
  assert available(pruned, "ana", at(1_000)) == 2
  assert available(pruned, "bo", at(1_000)) == 1
  (decisions, _) = replay(pruned, [("ana", at(1_000)), ("ana", at(1_000)), ("ana", at(1_000))])
  assert decisions == [Allowed(remaining: 1), Allowed(remaining: 0), Refused(retry_after: 1.seconds)]
end

test "a limit out of range is refused"
  assert limit(0, 1.0) == Error(BadCapacity(capacity: 0))
  assert limit(max_capacity + 1, 1.0) == Error(BadCapacity(capacity: max_capacity + 1))
  assert limit(1, 0.0) == Error(BadRate(per_second: 0.0))
  assert limit(1, -2.0) == Error(BadRate(per_second: -2.0))
  assert limit(1, 0.0000001) == Error(BadRate(per_second: 0.0000001))
  too_fast = (max_per_second + 1).to_decimal
  assert limit(1, too_fast) == Error(BadRate(per_second: too_fast))
  assert limit(max_capacity, 0.000001) is Ok(_)
  assert limit(1, max_per_second.to_decimal) is Ok(_)
end

test "the slowest rate makes a client wait a million seconds"
  (decisions, _) = replay(fresh(1, 0.000001), [("ana", at(0)), ("ana", at(0))])
  assert decisions == [Allowed(remaining: 0), Refused(retry_after: 1_000_000.seconds)]
end

test "the widest limit over the longest idle time still adds up exactly"
  assert Time.parse("0001-01-01T00:00:00Z") is Some(first_day)
  assert Time.parse("9999-12-31T23:59:59Z") is Some(last_day)
  (taken, used) = request(fresh(max_capacity, max_per_second.to_decimal), "ana", first_day)
  assert taken == Allowed(remaining: max_capacity - 1)
  assert available(used, "ana", last_day) == max_capacity.to_decimal
end
