## Service promises follow token; on names a tool and stays an ordinary name elsewhere.
service
  state Int = 0
  tools add
  token from env "TOKEN"

  promise "the count never falls"
    after >= before
    allows before: 0, after: 1, now: t0
    forbids before: 1, after: 0, now: t0
  end

  promise "an addition adds its amount" on add  # tool promise
    reply implies after == before + on
    allows before: 0, on: 1, reply: true, after: 1, now: t0
    forbids before: 0, on: 1, reply: true, after: 0, now: t0
  end
end

on = 1

fn on(on: Int) : Int
  promise "on is a name" on on
    result == on
    allows on: 1, result: 1
    forbids on: 1, result: 2
  end

  on
end
