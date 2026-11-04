## A fixture for the client probe: an author tool whose name a client could
## confuse with an operator tool's, and one ordinary tool.
module Main

service
  state Int = 0
  tools mo_state, count
  token from env "PROBE_TOKEN"
end

## The author's own tool that happens to be named like an operator view.
fn mo_state(state: Int, _now: Time) : Text
  "author tool mo_state, count #{state}"
end

## Adds one to the count and returns the new count.
fn count(state: Int, _now: Time) : (Int, Int)
  (state + 1, state + 1)
end
