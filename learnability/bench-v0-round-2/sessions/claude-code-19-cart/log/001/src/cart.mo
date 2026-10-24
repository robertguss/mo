## A shopping cart: its lines, its total and its receipt.
module Cart
expose Cart, Line(..), CodeError(..), empty, add, apply_code, subtotal, discount, total, receipt

struct Line
  name: Text
  price: Decimal
  quantity: Int
end

## The discount codes a cart can hold.
enum Code
  Save10
  FiveOff
  Bulk
end

## Why a code can't be put on a cart.
enum CodeError
  UnknownCode(code: Text)
  AlreadyApplied(code: Text)
end

struct Cart
  lines: List(Line)
  code: Option(Code)
end

fn empty() : Cart
  Cart(lines: [], code: None)
end

fn code_of(text: Text) : Option(Code)
  case text
    "SAVE10": Some(Save10)
    "FIVEOFF": Some(FiveOff)
    "BULK": Some(Bulk)
    _: None
  end
end

fn code_name(code: Code) : Text
  case code
    Save10: "SAVE10"
    FiveOff: "FIVEOFF"
    Bulk: "BULK"
  end
end

## Puts a discount code on the cart, which holds at most one.
fn apply_code(cart: Cart, code: Text) : Result(Cart, CodeError)
  examples
    apply_code(empty(), "SAVE10") is Ok(_)
    apply_code(empty(), "save10") == Error(UnknownCode(code: "save10"))
  end

  known = try code_of(code).or_fail(UnknownCode(code: code))
  case cart.code
    Some(held): Error(AlreadyApplied(code: code_name(held)))
    None: Ok(Cart(lines: cart.lines, code: Some(known)))
  end
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

## What the cart's code takes off, rounded once to whole cents and never more than the subtotal.
fn discount(cart: Cart) : Decimal
  examples
    discount(empty()) == 0
  end

  raw = case cart.code
    None: 0.0
    Some(Save10): subtotal(cart) * 0.10
    Some(FiveOff): 5.0
    Some(Bulk):
      cart.lines
        .filter(fn(line) line.quantity >= 10 end)
        .reduce(0.0, fn(sum, line) sum + line_total(line) * 0.15 end)
  end
  raw.round(2).min(subtotal(cart))
end

fn total(cart: Cart) : Decimal
  subtotal(cart) - discount(cart)
end

fn money(amount: Decimal) : Text
  amount.to_fixed(2)
end

## The receipt: a line for each line of the cart, then the discount on a cart with a code, then
## the total.
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
  with_discount = case cart.code
    Some(code): rows.append("discount #{code_name(code)}: -#{money(discount(cart))}")
    None: rows
  end
  with_discount.append("total: #{money(total(cart))}")
end
