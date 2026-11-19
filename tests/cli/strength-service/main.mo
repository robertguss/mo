module Main

service
  state Int = initial()
  tools add
  token from env "COUNTER_TOKEN"
end

fn initial() : Int
  0
end

fn add(state: Int, _now: Time, amount: Int) : (Int, Unit)
  examples
    add(1, t0, 2) == (3, Unit)
  end

  (state + amount, Unit)
end

t0 = Time.parse("2026-01-01T00:00:00Z") or stop "time"
