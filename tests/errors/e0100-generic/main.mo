module Main
service
  state Int = 0
  tools echo
  token from env "TOKEN"
end
fn echo(_state: Int, _now: Time, value: T) : T
  value
end
