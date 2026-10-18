module Main

fn stamp(clock: Clock) : Text
  clock.now.to_text
end

fn later(clock: Clock, wait: Duration) : Option(Time)
  clock.now.checked_add(wait)
end

test "a fixed clock always says the same time"
  clock = Clock.fixed(Time.parse("2026-10-12T02:15:00Z") or stop "valid time")
  assert stamp(clock) == "2026-10-12T02:15:00Z"
  assert stamp(clock) == "2026-10-12T02:15:00Z"
end

test "adding a wait to the fixed time"
  clock = Clock.fixed(Time.parse("2026-10-12T02:15:00Z") or stop "valid time")
  assert (later(clock, 90.seconds) or stop "in range").to_text == "2026-10-12T02:16:30Z"
  assert (later(clock, 500.milliseconds) or stop "in range").to_text == "2026-10-12T02:15:00.5Z"
end
