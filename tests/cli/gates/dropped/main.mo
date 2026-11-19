module Main

t0 = Time.parse("2026-01-01T00:00:00Z") or stop "time"

fn elapsed(start: Time, finish: Time) : Duration
  examples
    elapsed(t0, t0).in_milliseconds == 0
  end

  finish - start
end

fn main()
end
