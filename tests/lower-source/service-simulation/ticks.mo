module Ticks

struct History
  entries: List((Time, Int))
end

service
  state History = History(entries: [])
  tools history
  every 1.minute first
  every 2.minutes second
  token from env "TOKEN"
end

fn first(state: History, now: Time) : History
  History(entries: state.entries.append((now, 1)))
end

fn second(state: History, now: Time) : History
  History(entries: state.entries.append((now, 2)))
end

fn history(state: History, _now: Time) : List((Time, Int))
  state.entries
end
