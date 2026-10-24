## Tests of the Cart module.
module CartTest
use Cart{Cart, Line, CodeError}

fn cart_of(lines: List(Line)) : Cart
  lines.reduce(Cart.empty(), fn(cart, line) Cart.add(cart, line) end)
end

test "an empty cart costs nothing"
  assert Cart.total(Cart.empty()) == 0
  assert Cart.receipt(Cart.empty()) == ["total: 0.00"]
end

test "lines add up"
  cart = cart_of([
    Line(name: "tea", price: 3.50, quantity: 2),
    Line(name: "jam", price: 4, quantity: 1)
  ])
  assert Cart.subtotal(cart) == 11
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
  assert Cart.apply_code(original, "SAVE10") is Ok(cart)
  for code in ["SAVE10", "FIVEOFF", "BULK"]
    assert Cart.apply_code(cart, code) == Error(AlreadyApplied(code: "SAVE10"))
  end
  assert Cart.apply_code(cart, "save10") == Error(UnknownCode(code: "save10"))
end
