## Tests of the Cart module.
module CartTest
use Cart{Cart, Line, CodeError}

fn cart_of(lines: List(Line)) : Cart
  lines.reduce(Cart.empty(), fn(cart, line) Cart.add(cart, line) end)
end

test "an empty cart costs nothing"
  assert Cart.total(Cart.empty()) == 0
  assert Cart.discount(Cart.empty()) == 0
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

test "SAVE10 rounds a half cent away from zero and follows later additions"
  original = cart_of([Line(name: "tea", price: 10.05, quantity: 1)])
  assert Cart.apply_code(original, "SAVE10") is Ok(coded)
  assert Cart.subtotal(coded) == 10.05
  assert Cart.discount(coded) == 1.01
  assert Cart.total(coded) == 9.04
  assert Cart.receipt(coded) == ["1 x tea @ 10.05 = 10.05", "discount SAVE10: -1.01", "total: 9.04"]
  enlarged = Cart.add(coded, Line(name: "sweet", price: 0.10, quantity: 1))
  assert Cart.discount(enlarged) == 1.02
  assert Cart.total(enlarged) == 9.13
  assert Cart.discount(original) == 0
  assert Cart.discount(coded) == 1.01
end

test "SAVE10 rounds the subtotal discount rather than each line"
  cart = cart_of([
    Line(name: "a", price: 0.04, quantity: 1),
    Line(name: "b", price: 0.04, quantity: 1)
  ])
  assert Cart.apply_code(cart, "SAVE10") is Ok(coded)
  assert Cart.discount(coded) == 0.01
  assert Cart.total(coded) == 0.07
end

test "FIVEOFF takes five dollars but never more than the subtotal"
  for (price, expected_discount, expected_total) in [
    (12.34, 5.00, 7.34),
    (5.00, 5.00, 0.00),
    (2.75, 2.75, 0.00)
  ]
    cart = cart_of([Line(name: "tea", price: price, quantity: 1)])
    assert Cart.apply_code(cart, "FIVEOFF") is Ok(coded)
    assert Cart.discount(coded) == expected_discount
    assert Cart.total(coded) == expected_total
  end
  assert Cart.apply_code(Cart.empty(), "FIVEOFF") is Ok(empty_coded)
  enlarged = Cart.add(empty_coded, Line(name: "tea", price: 8, quantity: 1))
  assert Cart.discount(enlarged) == 5
  assert Cart.receipt(enlarged) == [
    "1 x tea @ 8.00 = 8.00",
    "discount FIVEOFF: -5.00",
    "total: 3.00"
  ]
end

test "BULK discounts only lines with at least ten units"
  cart = cart_of([
    Line(name: "nine", price: 2, quantity: 9),
    Line(name: "ten", price: 0.01, quantity: 10),
    Line(name: "eleven", price: 0.02, quantity: 11)
  ])
  assert Cart.apply_code(cart, "BULK") is Ok(coded)
  assert Cart.subtotal(coded) == 18.32
  assert Cart.discount(coded) == 0.05
  assert Cart.total(coded) == 18.27
end

test "BULK rounds once after combining qualifying line discounts"
  cart = cart_of([
    Line(name: "a", price: 0.03, quantity: 10),
    Line(name: "b", price: 0.01, quantity: 10)
  ])
  assert Cart.apply_code(cart, "BULK") is Ok(coded)
  assert Cart.discount(coded) == 0.06
  assert Cart.total(coded) == 0.34
  assert Cart.apply_code(cart_of([Line(name: "a", price: 0.03, quantity: 10)]), "BULK") is Ok(half)
  assert Cart.discount(half) == 0.05
end

test "BULK follows merged quantities and new lines added after applying"
  cart = cart_of([Line(name: "tea", price: 2, quantity: 9)])
  assert Cart.apply_code(cart, "BULK") is Ok(coded)
  assert Cart.discount(coded) == 0
  merged = Cart.add(coded, Line(name: "tea", price: 2, quantity: 1))
  assert Cart.discount(merged) == 3
  enlarged = Cart.add(merged, Line(name: "jam", price: 1, quantity: 10))
  assert Cart.discount(enlarged) == 4.5
  assert Cart.receipt(enlarged) == [
    "10 x tea @ 2.00 = 20.00",
    "10 x jam @ 1.00 = 10.00",
    "discount BULK: -4.50",
    "total: 25.50"
  ]
end

test "codes are exact and unknown codes take precedence over an existing code"
  for unknown in ["save10", "FIVEOFF ", " BULK", "", "OTHER"]
    assert Cart.apply_code(Cart.empty(), unknown) == Error(UnknownCode(code: unknown))
    assert Cart.apply_code(Cart.empty(), "SAVE10") is Ok(coded)
    assert Cart.apply_code(coded, unknown) == Error(UnknownCode(code: unknown))
  end
end

test "each code prevents another known code and reports the existing one"
  for existing in ["SAVE10", "FIVEOFF", "BULK"]
    assert Cart.apply_code(Cart.empty(), existing) is Ok(coded)
    for attempted in ["SAVE10", "FIVEOFF", "BULK"]
      assert Cart.apply_code(coded, attempted) == Error(AlreadyApplied(code: existing))
    end
  end
end

test "all codes on empty carts show a zero discount before the total"
  for code in ["SAVE10", "FIVEOFF", "BULK"]
    assert Cart.apply_code(Cart.empty(), code) is Ok(coded)
    assert Cart.discount(coded) == 0
    assert Cart.total(coded) == 0
    assert Cart.receipt(coded) == ["discount #{code}: -0.00", "total: 0.00"]
  end
end
