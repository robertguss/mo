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

test "a cart without a code has no discount"
  cart = cart_of([Line(name: "tea", price: 3.50, quantity: 20)])
  assert Cart.discount(cart) == 0
  assert Cart.total(cart) == 70
end

test "SAVE10 takes ten percent off the subtotal"
  cart = coded(
    cart_of([Line(name: "tea", price: 3.50, quantity: 2), Line(name: "jam", price: 4, quantity: 1)]),
    "SAVE10"
  )
  assert Cart.discount(cart) == 1.10
  assert Cart.total(cart) == 9.90
  assert Cart.receipt(cart) == [
    "2 x tea @ 3.50 = 7.00",
    "1 x jam @ 4.00 = 4.00",
    "discount SAVE10: -1.10",
    "total: 9.90"
  ]
end

test "the discount is rounded once to whole cents, a half away from zero"
  cart = coded(cart_of([Line(name: "gum", price: 0.05, quantity: 1)]), "SAVE10")
  assert Cart.discount(cart) == 0.01
  three = coded(
    cart_of([
      Line(name: "a", price: 0.05, quantity: 1),
      Line(name: "b", price: 0.05, quantity: 1),
      Line(name: "c", price: 0.05, quantity: 1)
    ]),
    "SAVE10"
  )
  assert Cart.discount(three) == 0.02
end

test "FIVEOFF takes 5.00 off, but never more than the subtotal"
  big = coded(cart_of([Line(name: "jam", price: 4, quantity: 3)]), "FIVEOFF")
  assert Cart.discount(big) == 5
  assert Cart.total(big) == 7
  small = coded(cart_of([Line(name: "gum", price: 1.25, quantity: 2)]), "FIVEOFF")
  assert Cart.discount(small) == 2.50
  assert Cart.total(small) == 0
  assert Cart.receipt(small) == ["2 x gum @ 1.25 = 2.50", "discount FIVEOFF: -2.50", "total: 0.00"]
  assert Cart.discount(coded(Cart.empty(), "FIVEOFF")) == 0
end

test "BULK takes fifteen percent off only lines of 10 or more"
  cart = coded(
    cart_of([Line(name: "tea", price: 3.33, quantity: 10), Line(name: "jam", price: 4, quantity: 9)]),
    "BULK"
  )
  assert Cart.discount(cart) == 5.00
  assert Cart.total(cart) == 64.30
  none = coded(cart_of([Line(name: "jam", price: 4, quantity: 9)]), "BULK")
  assert Cart.discount(none) == 0
  assert Cart.receipt(none) == ["9 x jam @ 4.00 = 36.00", "discount BULK: -0.00", "total: 36.00"]
end

test "BULK rounds the sum of its lines once, not each line"
  cart = coded(
    cart_of([
      Line(name: "a", price: 0.01, quantity: 10),
      Line(name: "b", price: 0.01, quantity: 10),
      Line(name: "c", price: 0.01, quantity: 10)
    ]),
    "BULK"
  )
  assert Cart.discount(cart) == 0.05
end

test "the discount follows lines added after the code"
  cart = coded(cart_of([Line(name: "tea", price: 1, quantity: 9)]), "BULK")
  assert Cart.discount(cart) == 0
  more = Cart.add(cart, Line(name: "tea", price: 1, quantity: 1))
  assert Cart.discount(more) == 1.50
  assert Cart.total(more) == 8.50
end

test "codes are matched exactly"
  assert Cart.apply_code(Cart.empty(), "save10") == Error(UnknownCode(code: "save10"))
  assert Cart.apply_code(Cart.empty(), " SAVE10") == Error(UnknownCode(code: " SAVE10"))
  assert Cart.apply_code(Cart.empty(), "") == Error(UnknownCode(code: ""))
end

test "a cart holds at most one code"
  cart = coded(Cart.empty(), "SAVE10")
  assert Cart.apply_code(cart, "FIVEOFF") == Error(AlreadyApplied(code: "SAVE10"))
  assert Cart.apply_code(cart, "SAVE10") == Error(AlreadyApplied(code: "SAVE10"))
  assert Cart.apply_code(cart, "NOPE") == Error(UnknownCode(code: "NOPE"))
end
