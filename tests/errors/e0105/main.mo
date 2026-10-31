module Main
use Types{Hidden, Visible}
service
  state Int = 0
  tools visible, snapshot
  token from env "TOKEN"
end
fn visible(_state: Int, _now: Time, value: Visible) : Option(Visible)
  Some(value)
end
fn snapshot(_state: Int, _now: Time, _value: Hidden) : Bool
  true
end
