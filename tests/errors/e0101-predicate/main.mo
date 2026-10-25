module Main
service
  state Bool = false
  tools active?
  token from env "TOKEN"
end
fn active?(state: Bool, _now: Time) : Bool
  not state
end
