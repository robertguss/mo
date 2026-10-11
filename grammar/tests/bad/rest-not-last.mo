# error: ..rest can only end a list pattern
case xs
  [..rest, last]: last
end
