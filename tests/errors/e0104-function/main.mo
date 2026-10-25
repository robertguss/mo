module Main
service
  state Int = 0
  tools snapshot
  token from env "TOKEN"
end
fn snapshot(_state: Int, _now: Time) : Option(fn() : Int)
  None
end
