## The second service: it keeps every note and counts the minutes.
module Tally
expose step

step = 1

service
  state (List(Text), Int) = ([], 0)
  tools note
  every 5.minutes tick
  token from env "TALLY_TOKEN"
end

fn note(state: (List(Text), Int), _now: Time, text: Text) : ((List(Text), Int), Int)
  (notes, ticks) = state
  ((notes.append(text), ticks), notes.size + 1)
end

fn tick(state: (List(Text), Int), _now: Time) : (List(Text), Int)
  (notes, ticks) = state
  (notes, ticks + 1)
end
