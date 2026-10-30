module Main
use Types{Private, Hidden}
service
  state Option(Private) = None
  tools update, snapshot
  token from env "TOKEN"
end
fn update(state: Option(Private), _now: Time) : Result((Option(Private), Bool), Text)
  Ok((state, true))
end
fn snapshot(state: Option(Private), _now: Time) : Result((Option(Private), Bool), Hidden)
  Ok((state, true))
end
