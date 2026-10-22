## The hidden tests of task 19-cart.
module HiddenTests
use Cart{Cart, Line, CodeError}

fn cart_of(lines: List(Line)) : Cart
  lines.reduce(Cart.empty(), fn(cart, line) Cart.add(cart, line) end)
end

fn with_code(cart: Cart, code: Text) : Cart
  case Cart.apply_code(cart, code)
    Ok(coded): coded
    Error(_): stop "the tests apply known codes once"
  end
end

tea = Line(name: "tea", price: 3.50, quantity: 2)
jam = Line(name: "jam", price: 4, quantity: 1)

test "SAVE10 takes ten percent off"
  cart = with_code(cart_of([tea, jam]), "SAVE10")
  assert Cart.discount(cart) == 1.1
  assert Cart.total(cart) == 9.9
  assert Cart.subtotal(cart) == 11
end

test "the discount is rounded to whole cents, a half away from zero"
  cart = with_code(cart_of([Line(name: "gum", price: 0.25, quantity: 1)]), "SAVE10")
  assert Cart.discount(cart) == 0.03
  assert Cart.total(cart) == 0.22
  odd = with_code(cart_of([Line(name: "pen", price: 1.33, quantity: 1)]), "SAVE10")
  assert Cart.discount(odd) == 0.13
end

test "FIVEOFF takes five off, but never below zero"
  assert Cart.total(with_code(cart_of([tea, jam]), "FIVEOFF")) == 6
  small = with_code(cart_of([jam]), "FIVEOFF")
  assert Cart.discount(small) == 4
  assert Cart.total(small) == 0
  assert Cart.total(with_code(Cart.empty(), "FIVEOFF")) == 0
end

test "BULK takes fifteen percent off each line of ten or more"
  cart = cart_of([
    Line(name: "egg", price: 0.30, quantity: 12),
    tea,
    Line(name: "nut", price: 1, quantity: 10)
  ])
  coded = with_code(cart, "BULK")
  assert Cart.discount(coded) == 2.04
  assert Cart.total(coded) == 18.56
  assert Cart.discount(with_code(cart_of([tea, jam]), "BULK")) == 0
end

test "BULK counts a line's quantity once lines are merged"
  cart = cart_of([
    Line(name: "egg", price: 1, quantity: 6),
    Line(name: "egg", price: 1, quantity: 4)
  ])
  assert Cart.discount(with_code(cart, "BULK")) == 1.5
end

test "the discount follows lines added after the code"
  coded = with_code(cart_of([jam]), "SAVE10")
  more = Cart.add(coded, tea)
  assert Cart.discount(more) == 1.1
  assert Cart.total(more) == 9.9
end

test "codes are known and exact"
  assert Cart.apply_code(Cart.empty(), "SAVE20") == Error(UnknownCode(code: "SAVE20"))
  assert Cart.apply_code(Cart.empty(), "save10") == Error(UnknownCode(code: "save10"))
  assert Cart.apply_code(Cart.empty(), "") == Error(UnknownCode(code: ""))
end

test "a cart takes one code"
  coded = with_code(cart_of([tea]), "SAVE10")
  assert Cart.apply_code(coded, "FIVEOFF") == Error(AlreadyApplied(code: "SAVE10"))
  assert Cart.apply_code(coded, "SAVE10") == Error(AlreadyApplied(code: "SAVE10"))
  assert Cart.apply_code(coded, "NOPE") == Error(UnknownCode(code: "NOPE"))
end

test "the receipt shows the discount before the total"
  cart = with_code(cart_of([tea, jam]), "SAVE10")
  assert Cart.receipt(cart) == [
    "2 x tea @ 3.50 = 7.00",
    "1 x jam @ 4.00 = 4.00",
    "discount SAVE10: -1.10",
    "total: 9.90"
  ]
  assert Cart.receipt(with_code(cart_of([jam]), "BULK")) == [
    "1 x jam @ 4.00 = 4.00",
    "discount BULK: -0.00",
    "total: 4.00"
  ]
end

test "a cart without a code has no discount"
  assert Cart.discount(cart_of([tea, jam])) == 0
  assert Cart.receipt(cart_of([jam])) == ["1 x jam @ 4.00 = 4.00", "total: 4.00"]
end
