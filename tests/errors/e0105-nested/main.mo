module Main
use Types{Reply}
service
  state Int = 0
  tools snapshot
  token from env "TOKEN"
end
fn snapshot(_state: Int, _now: Time) : Option(Reply)
  None
end
