module Main
use Types{Private, Visible, Reply}
service
  state Option(Private) = None
  tools read, update, try_update, snapshot
  every 1.minute tick
  token from env "TOKEN"
end
fn read(_state: Option(Private), _now: Time, value: Visible(Int)) : Option(Visible(Int))
  Some(value)
end
fn update(state: Option(Private), _now: Time) : (Option(Private), Bool)
  (state, true)
end
fn try_update(state: Option(Private), _now: Time) : Result((Option(Private), Bool), Text)
  Ok((state, true))
end
fn snapshot(state: Option(Private), _now: Time) : (Option(Private), Reply)
  (state, Reply(value: None))
end
fn tick(state: Option(Private), _now: Time) : Option(Private)
  state
end
