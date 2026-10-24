module Main

fn either(a: Bool, b: Bool) : Bool
  var c = a
  c ||= b
  c
end
