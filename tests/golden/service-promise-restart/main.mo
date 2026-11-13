## The tidy tick loses every number when it runs off a minute boundary, which happens only at
## the startup tick of a restart after the clock moved. Recovery itself is exact, so only the
## service promise sees the loss.
module Main

service
  state List(Int) = []
  tools add
  every 1.minute tidy
  token from env "NUMBERS_TOKEN"

  promise "no number is ever lost"
    before.all?(fn(n) after.contains?(n) end)
    allows before: [1], after: [1, 2], now: t0
    forbids before: [1], after: [], now: t0
  end
end

fn add(numbers: List(Int), _now: Time, n: Int) : (List(Int), Int)
  (numbers.append(n), numbers.size + 1)
end

fn tidy(numbers: List(Int), now: Time) : List(Int)
  return numbers if (now - t0).in_milliseconds % 60000 == 0
  []
end

t0 = Time.parse("2026-01-01T00:00:00Z") or stop "time"
