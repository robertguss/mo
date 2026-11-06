module Main

struct Holder
  count: Int
  write: fn(Text) : Int
end

service
  state Int = 0
  tools add
  token from env "TOKEN"
end

fn add(state: Int, _now: Time) : (Int, Bool)
  (state + 1, true)
end

fn peek(state: Int, holder: Holder) : Int
  state + holder.count
end
