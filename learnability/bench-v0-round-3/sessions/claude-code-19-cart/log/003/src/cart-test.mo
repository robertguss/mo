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

test "SAVE10 takes ten percent off the subtotal"
  cart = cart_of([
    Line(name: "tea", price: 3.5, quantity: 2),
    Line(name: "jam", price: 4, quantity: 1)
  ])
  assert Cart.apply_code(cart, "SAVE10") is Ok(saved)
  assert Cart.subtotal(saved) == 11
  assert Cart.discount(saved) == 1.10
  assert Cart.total(saved) == 9.90
  assert Cart.receipt(saved) == [
    "2 x tea @ 3.50 = 7.00",
    "1 x jam @ 4.00 = 4.00",
    "discount SAVE10: -1.10",
    "total: 9.90"
  ]
end

test "the discount is rounded to whole cents, a half away from zero"
  cart = cart_of([Line(name: "mint", price: 0.25, quantity: 1)])
  assert Cart.apply_code(cart, "SAVE10") is Ok(saved)
  assert Cart.discount(saved) == 0.03
  assert Cart.total(saved) == 0.22
end

test "FIVEOFF takes 5.00 off, but never more than the subtotal"
  big = cart_of([Line(name: "jam", price: 4, quantity: 3)])
  assert Cart.apply_code(big, "FIVEOFF") is Ok(off)
  assert Cart.discount(off) == 5
  assert Cart.total(off) == 7
  small = cart_of([Line(name: "tea", price: 3, quantity: 1)])
  assert Cart.apply_code(small, "FIVEOFF") is Ok(capped)
  assert Cart.discount(capped) == 3
  assert Cart.total(capped) == 0
  assert Cart.receipt(capped) == ["1 x tea @ 3.00 = 3.00", "discount FIVEOFF: -3.00", "total: 0.00"]
  assert Cart.apply_code(Cart.empty(), "FIVEOFF") is Ok(nothing)
  assert Cart.discount(nothing) == 0
  assert Cart.receipt(nothing) == ["discount FIVEOFF: -0.00", "total: 0.00"]
end

test "BULK takes fifteen percent off only lines of 10 or more"
  cart = cart_of([
    Line(name: "egg", price: 1.25, quantity: 10),
    Line(name: "jam", price: 4, quantity: 9)
  ])
  assert Cart.apply_code(cart, "BULK") is Ok(bulk)
  assert Cart.subtotal(bulk) == 48.50
  assert Cart.discount(bulk) == 1.88
  assert Cart.total(bulk) == 46.62
  assert Cart.receipt(bulk) == [
    "10 x egg @ 1.25 = 12.50",
    "9 x jam @ 4.00 = 36.00",
    "discount BULK: -1.88",
    "total: 46.62"
  ]
end

test "BULK rounds once over all lines, not line by line"
  cart = cart_of([
    Line(name: "pin", price: 0.01, quantity: 10),
    Line(name: "nail", price: 0.01, quantity: 10)
  ])
  assert Cart.apply_code(cart, "BULK") is Ok(bulk)
  assert Cart.discount(bulk) == 0.03
end

test "the discount counts lines added after the code"
  assert Cart.apply_code(Cart.empty(), "BULK") is Ok(bulk)
  assert Cart.discount(bulk) == 0
  more = Cart.add(
    Cart.add(bulk, Line(name: "egg", price: 2, quantity: 6)),
    Line(name: "egg", price: 2, quantity: 4)
  )
  assert Cart.discount(more) == 3
  assert Cart.total(more) == 17
end

test "codes are matched exactly"
  assert Cart.apply_code(Cart.empty(), "save10") == Error(UnknownCode(code: "save10"))
  assert Cart.apply_code(Cart.empty(), " SAVE10") == Error(UnknownCode(code: " SAVE10"))
  assert Cart.apply_code(Cart.empty(), "") == Error(UnknownCode(code: ""))
end

test "a cart holds at most one code"
  cart = cart_of([Line(name: "tea", price: 3, quantity: 10)])
  assert Cart.apply_code(cart, "SAVE10") is Ok(saved)
  assert Cart.apply_code(saved, "BULK") == Error(AlreadyApplied(code: "SAVE10"))
  assert Cart.apply_code(saved, "SAVE10") == Error(AlreadyApplied(code: "SAVE10"))
  assert Cart.apply_code(saved, "HALFOFF") == Error(UnknownCode(code: "HALFOFF"))
  assert Cart.discount(saved) == 3
end
