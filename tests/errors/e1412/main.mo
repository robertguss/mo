module Main

service
  state Int = 0
  tools add
  token from env "TOKEN"
end

fn add(state: Int, _now: Time) : (Int, Bool)
  (state + 1, true)
end

fn double(text: Text) : Text
  "#{text}#{text}"
end
