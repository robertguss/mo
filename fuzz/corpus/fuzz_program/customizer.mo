module Main

struct Box(T)
  value: T
end

fn to_text(_box: Box(T)) : Text where T: Text
  "box"
end

fn supported?(_value: T) : Bool where T: Text
  true
end

fn main() : Int
  if supported?(Box(value: Box(value: 1))): 1 else: 0
end
