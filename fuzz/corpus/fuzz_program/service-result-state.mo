module Main
service
  state Int = 0
  tools change
  token from env "TOKEN"
end
fn change(state: Int, _now: Time) : Result(Int, Text)
  Ok(state + 1)
end
