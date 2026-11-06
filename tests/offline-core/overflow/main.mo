module Main
struct Counter
  count: Int
  step: Int
end
service
  state Counter = Counter(count: 9223372036854775000, step: 800)
  tools bump
  token from env "OPERATOR_TOKEN"
end
fn bump(state: Counter, _now: Time, times: Int) : (Counter, Int)
  next = state.count + state.step * times
  (Counter(count: next, step: state.step), next)
end
