module Checks
use Main

test "half of ten is five"
  assert Main.half(10) == 5
end

test "true and false is false"
  assert Main.both(true, false) == false
end
