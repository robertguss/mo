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
    Error(_): stop "the tests apply known codes once"
  end
end

test "each code takes its discount off the total"
  cart = cart_of([
    Line(name: "tea", price: 3.5, quantity: 2),
    Line(name: "jam", price: 4, quantity: 1)
  ])
  assert Cart.total(coded(cart, "SAVE10")) == 9.9
  assert Cart.total(coded(cart, "FIVEOFF")) == 6
  assert Cart.discount(coded(cart_of([Line(name: "egg", price: 0.3, quantity: 12)]), "BULK")) ==
    0.54
end

test "a discount never takes the total below zero"
  assert Cart.total(coded(cart_of([Line(name: "jam", price: 4, quantity: 1)]), "FIVEOFF")) == 0
end

test "codes are exact, and a cart takes one"
  assert Cart.apply_code(Cart.empty(), "save10") == Error(UnknownCode(code: "save10"))
  assert Cart.apply_code(coded(Cart.empty(), "BULK"), "SAVE10") ==
    Error(AlreadyApplied(code: "BULK"))
end

test "the receipt shows the discount before the total"
  cart = coded(cart_of([Line(name: "jam", price: 4, quantity: 1)]), "SAVE10")
  assert Cart.receipt(cart) == ["1 x jam @ 4.00 = 4.00", "discount SAVE10: -0.40", "total: 3.60"]
end
