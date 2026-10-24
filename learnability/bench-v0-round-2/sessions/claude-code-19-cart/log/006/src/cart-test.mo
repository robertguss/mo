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

fn coded(cart: Cart, code: Text) : Cart
  case Cart.apply_code(cart, code)
    Ok(with_code): with_code
    Error(_): stop "the tests apply only known codes to carts without one"
  end
end

test "SAVE10 takes ten percent off the subtotal"
  cart = coded(
    cart_of([Line(name: "tea", price: 3.50, quantity: 2), Line(name: "jam", price: 4, quantity: 1)]),
    "SAVE10"
  )
  assert Cart.subtotal(cart) == 11
  assert Cart.discount(cart) == 1.10
  assert Cart.total(cart) == 9.90
  assert Cart.receipt(cart) == [
    "2 x tea @ 3.50 = 7.00",
    "1 x jam @ 4.00 = 4.00",
    "discount SAVE10: -1.10",
    "total: 9.90"
  ]
end

test "SAVE10 rounds a half cent away from zero"
  cart = coded(cart_of([Line(name: "gum", price: 0.05, quantity: 1)]), "SAVE10")
  assert Cart.discount(cart) == 0.01
  assert Cart.total(cart) == 0.04
end

test "FIVEOFF takes 5.00 off, but never more than the subtotal"
  big = coded(cart_of([Line(name: "jam", price: 4, quantity: 3)]), "FIVEOFF")
  assert Cart.discount(big) == 5
  assert Cart.total(big) == 7
  small = coded(cart_of([Line(name: "tea", price: 3, quantity: 1)]), "FIVEOFF")
  assert Cart.discount(small) == 3
  assert Cart.total(small) == 0
  assert Cart.receipt(small) == ["1 x tea @ 3.00 = 3.00", "discount FIVEOFF: -3.00", "total: 0.00"]
  assert Cart.discount(coded(Cart.empty(), "FIVEOFF")) == 0
end

test "BULK takes fifteen percent off lines of 10 or more only"
  cart = coded(
    cart_of([Line(name: "egg", price: 2, quantity: 10), Line(name: "jam", price: 4, quantity: 9)]),
    "BULK"
  )
  assert Cart.discount(cart) == 3
  assert Cart.total(cart) == 53
  assert Cart.receipt(cart) == [
    "10 x egg @ 2.00 = 20.00",
    "9 x jam @ 4.00 = 36.00",
    "discount BULK: -3.00",
    "total: 53.00"
  ]
end

test "BULK rounds once, over all its lines"
  cart = coded(
    cart_of([
      Line(name: "pin", price: 0.33, quantity: 10),
      Line(name: "nut", price: 0.33, quantity: 10)
    ]),
    "BULK"
  )
  assert Cart.discount(cart) == 0.99
  assert Cart.total(cart) == 5.61
end

test "BULK on a cart with no line of 10 takes nothing off"
  cart = coded(cart_of([Line(name: "tea", price: 3, quantity: 9)]), "BULK")
  assert Cart.discount(cart) == 0
  assert Cart.receipt(cart) == ["9 x tea @ 3.00 = 27.00", "discount BULK: -0.00", "total: 27.00"]
end

test "the discount counts lines added after the code"
  cart = coded(cart_of([Line(name: "egg", price: 2, quantity: 5)]), "BULK")
  assert Cart.discount(cart) == 0
  more = Cart.add(cart, Line(name: "egg", price: 2, quantity: 5))
  assert Cart.discount(more) == 3
  assert Cart.total(more) == 17
  saved = Cart.add(coded(Cart.empty(), "SAVE10"), Line(name: "jam", price: 4, quantity: 1))
  assert Cart.total(saved) == 3.60
end

test "a cart without a code has no discount line"
  cart = cart_of([Line(name: "tea", price: 3, quantity: 1)])
  assert Cart.discount(cart) == 0
  assert Cart.receipt(cart) == ["1 x tea @ 3.00 = 3.00", "total: 3.00"]
end

test "codes are matched exactly"
  assert Cart.apply_code(Cart.empty(), "save10") == Error(UnknownCode(code: "save10"))
  assert Cart.apply_code(Cart.empty(), " SAVE10") == Error(UnknownCode(code: " SAVE10"))
  assert Cart.apply_code(Cart.empty(), "") == Error(UnknownCode(code: ""))
end

test "a cart holds at most one code"
  cart = coded(Cart.empty(), "FIVEOFF")
  assert Cart.apply_code(cart, "SAVE10") == Error(AlreadyApplied(code: "FIVEOFF"))
  assert Cart.apply_code(cart, "FIVEOFF") == Error(AlreadyApplied(code: "FIVEOFF"))
  assert Cart.apply_code(cart, "NOPE") == Error(UnknownCode(code: "NOPE"))
end
