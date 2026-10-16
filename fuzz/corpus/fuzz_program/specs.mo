module Main
test "private \u{3bb}\u{0}"
  assert twice(3) == 6
end
fn twice(value: T) : T where T: Number, T: Equal
  promise "returns twice its input"
    result == value + value
    allows result: 6, value: 3
    forbids value: 1.25, result: 2.0
  end
  value + value
end
fn main()
end
