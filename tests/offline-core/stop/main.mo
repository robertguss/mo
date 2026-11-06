module Main
struct Tally
  total: Int
  parts: Int
end
service
  state Tally = Tally(total: 10, parts: 3)
  tools halt
  token from env "OPERATOR_TOKEN"
end
fn halt(state: Tally, _now: Time, n: Int) : Bool
  if n > state.total - state.parts
    stop "more than the room left"
  end
  true
end
