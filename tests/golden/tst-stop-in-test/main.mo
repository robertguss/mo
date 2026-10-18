module Main

fn halve(n: Int) : Int
  if n % 2 != 0: stop "odd numbers can't be halved" else: n / 2
end

test "halves an even number"
  assert halve(8) == 4
end

test "an odd number stops"
  assert halve(7) == 3
end
