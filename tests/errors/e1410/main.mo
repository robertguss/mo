module Main

service
  state List(Int) = []
  tools add
  token from env "TOKEN"
end

fn add(state: List(Int), _now: Time) : (List(Int), Bool)
  (state.append(1), true)
end

fn size(items: List(T)) : Int
  items.size
end
