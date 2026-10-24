## Tests of the Cart module.
module CartTest
use Cart{Cart, Line, CodeError}

fn cart_of(lines: List(Line)) : Cart
  lines.reduce(Cart.empty(), fn(cart, line) Cart.add(cart, line) end)
end

test "an empty cart costs nothing"
  assert Cart.discount(Cart.empty()) == 0
  assert Cart.total(Cart.empty()) == 0
  assert Cart.receipt(Cart.empty()) == ["total: 0.00"]
end

test "lines add up"
  cart = cart_of([
    Line(name: "tea", price: 3.50, quantity: 2),
    Line(name: "jam", price: 4, quantity: 1)
  ])
  assert Cart.subtotal(cart) == 11
  assert Cart.discount(cart) == 0
  assert Cart.total(cart) == 11
end

test "the same item added twice is one line"
  cart = cart_of([
    Line(name: "tea", price: 3.50, quantity: 2),
    Line(name: "tea", price: 3.50, quantity: 1)
  ])
  assert Cart.receipt(cart) == ["3 x tea @ 3.50 = 10.50", "total: 10.50"]
end

test "the receipt lists each line, then the total"
  cart = cart_of([
    Line(name: "tea", price: 3.5, quantity: 2),
    Line(name: "jam", price: 4, quantity: 1)
  ])
  assert Cart.receipt(cart) == ["2 x tea @ 3.50 = 7.00", "1 x jam @ 4.00 = 4.00", "total: 11.00"]
end

test "SAVE10 discounts the subtotal and appears just before the total"
  original = cart_of([
    Line(name: "tea", price: 3.50, quantity: 2),
    Line(name: "jam", price: 4, quantity: 1)
  ])
  assert Cart.apply_code(original, "SAVE10") is Ok(cart)
  assert Cart.subtotal(cart) == 11
  assert Cart.discount(cart) == 1.10
  assert Cart.total(cart) == 9.90
  assert Cart.receipt(cart) == [
    "2 x tea @ 3.50 = 7.00",
    "1 x jam @ 4.00 = 4.00",
    "discount SAVE10: -1.10",
    "total: 9.90"
  ]
  assert Cart.discount(original) == 0
  assert Cart.total(original) == 11
end

test "unknown codes take precedence over an already applied code"
  original = Cart.empty()
  for code in ["save10", "SAVE10 ", "", "NOPE"]
    assert Cart.apply_code(original, code) == Error(UnknownCode(code: code))
  end
  for applied in ["SAVE10", "FIVEOFF", "BULK"]
    assert Cart.apply_code(original, applied) is Ok(cart)
    for code in ["SAVE10", "FIVEOFF", "BULK"]
      assert Cart.apply_code(cart, code) == Error(AlreadyApplied(code: applied))
    end
    assert Cart.apply_code(cart, "save10") == Error(UnknownCode(code: "save10"))
  end
end

test "each code is accepted on an empty cart and prints a zero discount"
  for code in ["SAVE10", "FIVEOFF", "BULK"]
    assert Cart.apply_code(Cart.empty(), code) is Ok(cart)
    assert Cart.discount(cart) == 0
    assert Cart.total(cart) == 0
    assert Cart.receipt(cart) == ["discount #{code}: -0.00", "total: 0.00"]
  end
end

test "FIVEOFF is capped at the subtotal and follows later additions"
  assert Cart.apply_code(Cart.empty(), "FIVEOFF") is Ok(empty_cart)
  small = Cart.add(empty_cart, Line(name: "tea", price: 1.25, quantity: 1))
  assert Cart.discount(small) == 1.25
  assert Cart.total(small) == 0
  assert Cart.receipt(small) == ["1 x tea @ 1.25 = 1.25", "discount FIVEOFF: -1.25", "total: 0.00"]
  large = Cart.add(small, Line(name: "jam", price: 6, quantity: 1))
  assert Cart.subtotal(large) == 7.25
  assert Cart.discount(large) == 5
  assert Cart.total(large) == 2.25
  assert Cart.receipt(large) == [
    "1 x tea @ 1.25 = 1.25",
    "1 x jam @ 6.00 = 6.00",
    "discount FIVEOFF: -5.00",
    "total: 2.25"
  ]
end

test "SAVE10 rounds half cents away from zero and recalculates after additions"
  for (price, expected) in [(1.04, 0.10), (1.05, 0.11), (1.06, 0.11)]
    original = cart_of([Line(name: "tea", price: price, quantity: 1)])
    assert Cart.apply_code(original, "SAVE10") is Ok(cart)
    assert Cart.discount(cart) == expected
  end
  assert Cart.apply_code(Cart.empty(), "SAVE10") is Ok(empty_cart)
  first = Cart.add(empty_cart, Line(name: "tea", price: 0.03, quantity: 1))
  assert Cart.discount(first) == 0
  second = Cart.add(first, Line(name: "jam", price: 0.03, quantity: 1))
  assert Cart.discount(second) == 0.01
  assert Cart.total(second) == 0.05
end

test "BULK uses each current line quantity and keeps its code through additions"
  original = cart_of([Line(name: "tea", price: 2, quantity: 9)])
  assert Cart.apply_code(original, "BULK") is Ok(cart)
  assert Cart.discount(cart) == 0
  assert Cart.total(cart) == 18
  merged = Cart.add(cart, Line(name: "tea", price: 2, quantity: 1))
  assert Cart.discount(merged) == 3
  assert Cart.total(merged) == 17
  added = Cart.add(merged, Line(name: "jam", price: 3, quantity: 11))
  assert Cart.discount(added) == 7.95
  assert Cart.total(added) == 45.05
  different_price = Cart.add(added, Line(name: "tea", price: 2.50, quantity: 1))
  assert Cart.discount(different_price) == 7.95
  assert Cart.total(different_price) == 47.55
  assert Cart.receipt(different_price) == [
    "10 x tea @ 2.00 = 20.00",
    "11 x jam @ 3.00 = 33.00",
    "1 x tea @ 2.50 = 2.50",
    "discount BULK: -7.95",
    "total: 47.55"
  ]
  assert Cart.discount(cart) == 0
end

test "BULK rounds once across qualifying lines and includes quantity ten"
  original = cart_of([
    Line(name: "tea", price: 100, quantity: 9),
    Line(name: "jam", price: 0.01, quantity: 10),
    Line(name: "bread", price: 0.03, quantity: 11)
  ])
  assert Cart.apply_code(original, "BULK") is Ok(cart)
  assert Cart.discount(cart) == 0.06
  assert Cart.total(cart) == 900.37
  half_cent = cart_of([Line(name: "jam", price: 0.01, quantity: 10)])
  assert Cart.apply_code(half_cent, "BULK") is Ok(rounded)
  assert Cart.discount(rounded) == 0.02
  assert Cart.total(rounded) == 0.08
end
