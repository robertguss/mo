## A shopping cart: its lines, its total and its receipt.
module Cart
expose Cart, Line(..), CodeError(..), empty, add, apply_code, subtotal, discount, total, receipt

struct Line
  name: Text
  price: Decimal
  quantity: Int
end

struct Cart
  lines: List(Line)
  code: Option(Text)
end

## Why a discount code can't be applied.
enum CodeError
  UnknownCode(code: Text)
  AlreadyApplied(code: Text)
end

codes = ["SAVE10", "FIVEOFF", "BULK"]

fn empty() : Cart
  Cart(lines: [], code: None)
end

## Adds a line, or adds to the quantity of the line with the same name and price.
fn add(cart: Cart, line: Line) : Cart
  examples
    add(empty(), Line(name: "tea", price: 3, quantity: 1)).lines == [
      Line(name: "tea", price: 3, quantity: 1)
    ]
  end

  var lines = []
  var merged? = false
  for other in cart.lines
    if other.name == line.name and other.price == line.price
      lines = lines.append(
        Line(name: other.name, price: other.price, quantity: other.quantity + line.quantity)
      )
      merged? = true
    else
      lines = lines.append(other)
    end
  end
  if not merged?
    lines = lines.append(line)
  end
  Cart(lines: lines, code: cart.code)
end

fn line_total(line: Line) : Decimal
  line.price * line.quantity.to_decimal
end

fn subtotal(cart: Cart) : Decimal
  cart.lines.reduce(0.0, fn(sum, line) sum + line_total(line) end)
end

## Puts a discount code on the cart; a cart holds at most one.
fn apply_code(cart: Cart, code: Text) : Result(Cart, CodeError)
  examples
    apply_code(empty(), "SAVE10") is Ok(_)
    apply_code(empty(), "save10") == Error(UnknownCode(code: "save10"))
  end

  return Error(UnknownCode(code: code)) if not codes.contains?(code)
  case cart.code
    Some(applied): Error(AlreadyApplied(code: applied))
    None: Ok(Cart(lines: cart.lines, code: Some(code)))
  end
end

## What the cart's code takes off, in whole cents, and never more than the subtotal.
fn discount(cart: Cart) : Decimal
  examples
    discount(empty()) == 0
  end

  off = case cart.code
    Some("SAVE10"): subtotal(cart) * 0.10
    Some("FIVEOFF"): 5
    Some("BULK"):
      bulk = cart.lines.filter(fn(line) line.quantity >= 10 end)
      bulk.reduce(0.0, fn(sum, line) sum + line_total(line) * 0.15 end)
    Some(_) | None: 0
  end
  off.round(2)
end

fn total(cart: Cart) : Decimal
  subtotal(cart) - discount(cart)
end

fn money(amount: Decimal) : Text
  amount.to_fixed(2)
end

## The receipt: a line for each line of the cart, then the total.
fn receipt(cart: Cart) : List(Text)
  examples
    receipt(add(empty(), Line(name: "tea", price: 3, quantity: 2))) == [
      "2 x tea @ 3.00 = 6.00",
      "total: 6.00"
    ]
  end

  rows = cart.lines.map(fn(line)
    "#{line.quantity} x #{line.name} @ #{money(line.price)} = #{money(line_total(line))}"
  end)
  with_code = case cart.code
    Some(code): rows.append("discount #{code}: -#{money(discount(cart))}")
    None: rows
  end
  with_code.append("total: #{money(total(cart))}")
end
