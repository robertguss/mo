module Main
struct State
  values: List(Int)
end
service
  state State = (fn(x) fn(y) State(values: [x + y]) end end)(2)(1)
  tools update, snapshots
  every 1.minute tick
  token from env "TOKEN"
end
fn update(state: State, _now: Time, value: Int) : (State, Unit)
  (State(values: state.values.append(value)), Unit)
end
fn snapshots(state: State, _now: Time) : List(State)
  [state]
end
fn tick(state: State, _now: Time) : State
  state
end
