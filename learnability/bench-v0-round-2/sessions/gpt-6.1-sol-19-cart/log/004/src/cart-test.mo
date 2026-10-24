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

test "SAVE10 reduces the subtotal and adds a discount before the receipt total"
  original = cart_of([
    Line(name: "tea", price: 3.50, quantity: 2),
    Line(name: "jam", price: 4, quantity: 1)
  ])
  assert Cart.apply_code(original, "SAVE10") is Ok(cart)
  assert Cart.subtotal(cart) == 11
  assert Cart.discount(cart) == 1.10
  assert Cart.total(cart) == 9.90
  assert Cart.total(original) == 11
  assert Cart.receipt(cart) == [
    "2 x tea @ 3.50 = 7.00",
    "1 x jam @ 4.00 = 4.00",
    "discount SAVE10: -1.10",
    "total: 9.90"
  ]
end

test "SAVE10 rounds once to cents with halves away from zero"
  for (price, expected) in [(0.04, 0.00), (0.05, 0.01), (0.06, 0.01), (0.15, 0.02)]
    original = cart_of([Line(name: "tea", price: price, quantity: 1)])
    assert Cart.apply_code(original, "SAVE10") is Ok(cart)
    assert Cart.discount(cart) == expected
  end
  original = cart_of([
    Line(name: "tea", price: 0.05, quantity: 1),
    Line(name: "jam", price: 0.05, quantity: 1)
  ])
  assert Cart.apply_code(original, "SAVE10") is Ok(cart)
  assert Cart.discount(cart) == 0.01
  assert Cart.total(cart) == 0.09
end

test "FIVEOFF takes five off but never more than the subtotal"
  for (price, expected) in [(3.25, 3.25), (5.00, 5.00), (8.25, 5.00)]
    original = cart_of([Line(name: "tea", price: price, quantity: 1)])
    assert Cart.apply_code(original, "FIVEOFF") is Ok(cart)
    assert Cart.discount(cart) == expected
    assert Cart.total(cart) == price - expected
  end
  assert Cart.apply_code(Cart.empty(), "FIVEOFF") is Ok(empty_cart)
  assert Cart.discount(empty_cart) == 0
  assert Cart.receipt(empty_cart) == ["discount FIVEOFF: -0.00", "total: 0.00"]
end

test "BULK discounts only lines with at least ten units"
  original = cart_of([
    Line(name: "tea", price: 1.03, quantity: 9),
    Line(name: "jam", price: 1.03, quantity: 10),
    Line(name: "rice", price: 1.03, quantity: 11)
  ])
  assert Cart.apply_code(original, "BULK") is Ok(cart)
  assert Cart.subtotal(cart) == 30.90
  assert Cart.discount(cart) == 3.24
  assert Cart.total(cart) == 27.66
  assert Cart.receipt(cart) == [
    "9 x tea @ 1.03 = 9.27",
    "10 x jam @ 1.03 = 10.30",
    "11 x rice @ 1.03 = 11.33",
    "discount BULK: -3.24",
    "total: 27.66"
  ]
end

test "BULK rounds the combined discount rather than each line"
  original = cart_of([Line(name: "tea", price: 0.01, quantity: 10)])
  assert Cart.apply_code(original, "BULK") is Ok(cart)
  assert Cart.discount(cart) == 0.02
  expanded = Cart.add(cart, Line(name: "jam", price: 0.01, quantity: 10))
  assert Cart.discount(expanded) == 0.03
  assert Cart.total(expanded) == 0.17
end

test "codes use lines added later and BULK sees merged quantities"
  for (code, expected) in [("SAVE10", 2.00), ("FIVEOFF", 5.00), ("BULK", 3.00)]
    assert Cart.apply_code(Cart.empty(), code) is Ok(cart)
    assert Cart.discount(cart) == 0
    first = Cart.add(cart, Line(name: "tea", price: 2, quantity: 9))
    if code == "BULK"
      assert Cart.discount(first) == 0
    end
    expanded = Cart.add(first, Line(name: "tea", price: 2, quantity: 1))
    assert Cart.subtotal(expanded) == 20
    assert Cart.discount(expanded) == expected
    assert Cart.total(expanded) == 20 - expected
    assert Cart.receipt(expanded) == [
      "10 x tea @ 2.00 = 20.00",
      "discount #{code}: -#{expected.to_fixed(2)}",
      "total: #{(20 - expected).to_fixed(2)}"
    ]
  end
end

test "unknown codes are exact and take precedence over an already applied code"
  assert Cart.apply_code(Cart.empty(), "SAVE10") is Ok(cart)
  for code in ["save10", "FIVEOFF ", " BULK", "", "OTHER"]
    assert Cart.apply_code(Cart.empty(), code) == Error(UnknownCode(code: code))
    assert Cart.apply_code(cart, code) == Error(UnknownCode(code: code))
  end
end

test "a second valid code reports the existing code without replacing it"
  for existing in ["SAVE10", "FIVEOFF", "BULK"]
    assert Cart.apply_code(Cart.empty(), existing) is Ok(cart)
    for next in ["SAVE10", "FIVEOFF", "BULK"]
      assert Cart.apply_code(cart, next) == Error(AlreadyApplied(code: existing))
    end
    assert Cart.receipt(cart) == ["discount #{existing}: -0.00", "total: 0.00"]
  end
end
