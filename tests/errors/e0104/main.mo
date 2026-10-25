module Main
service
  state Option(Console) = None
  tools snapshot
  token from env "TOKEN"
end
fn snapshot(_state: Option(Console), _now: Time) : Bool
  true
end
