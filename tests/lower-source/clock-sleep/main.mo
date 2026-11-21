module Main

fn step_with(clock: Clock, pause: fn(Clock, Duration), d: Duration) : Time
  pause(clock, d)
  clock.now
end

fn main() : Int
  0
end

test "Clock.sleep lowers as dot call, module call and function value"
  t0 = Time.parse("2026-11-21T12:00:00Z") or stop "valid time"
  clock = Clock.fixed(t0)
  clock.sleep(250.milliseconds)
  Clock.sleep(clock, 750.milliseconds)
  after = step_with(clock, Clock.sleep, 4.seconds)
  assert after.to_text == "2026-11-21T12:00:05Z"
end
