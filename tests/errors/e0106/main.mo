module Main
service
  state Int = 0
  tools paid?
  token from env "TOKEN"
end
fn paid?(_state: Int, _now: Time) : Bool
  true
end
