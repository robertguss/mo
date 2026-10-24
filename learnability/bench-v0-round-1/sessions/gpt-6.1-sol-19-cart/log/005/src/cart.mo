## A shopping cart: its lines, its total and its receipt.
module Cart
expose Cart, Line(..), CodeError(..), empty, add, apply_code, subtotal, discount, total, receipt

struct Line
  name: Text
  price: Decimal
  quantity: Int
end

enum CodeError
  UnknownCode(code: Text)
  AlreadyApplied(code: Text)
end

enum DiscountCode
  Save10
  FiveOff
  Bulk
end

struct Cart
  lines: List(Line)
  code: Option(DiscountCode)
end

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

fn code_text(code: DiscountCode) : Text
  case code
    Save10: "SAVE10"
    FiveOff: "FIVEOFF"
    Bulk: "BULK"
  end
end

fn apply_code(cart: Cart, code: Text) : Result(Cart, CodeError)
  chosen = case code
    "SAVE10": Save10
    "FIVEOFF": FiveOff
    "BULK": Bulk
    _: return Error(UnknownCode(code: code))
  end
  case cart.code
    Some(existing): Error(AlreadyApplied(code: code_text(existing)))
    None: Ok(Cart(lines: cart.lines, code: Some(chosen)))
  end
end

fn line_total(line: Line) : Decimal
  line.price * line.quantity.to_decimal
end

fn subtotal(cart: Cart) : Decimal
  cart.lines.reduce(0.0, fn(sum, line) sum + line_total(line) end)
end

## Computes the discount from the current lines, rounding only the combined amount.
fn discount(cart: Cart) : Decimal
  amount = case cart.code
    None: return 0
    Some(Save10): subtotal(cart) * 0.10
    Some(FiveOff): 5.00
    Some(Bulk): cart.lines.reduce(
      0.0,
      fn(sum, line)
        if line.quantity >= 10: sum + line_total(line) * 0.15 else: sum
      end
    )
  end
  amount.round(2).min(subtotal(cart))
end

fn total(cart: Cart) : Decimal
  subtotal(cart) - discount(cart)
end

fn money(amount: Decimal) : Text
  amount.to_fixed(2)
end

## The receipt: each cart line, the applied discount if any, then the total.
fn receipt(cart: Cart) : List(Text)
  examples
    receipt(add(empty(), Line(name: "tea", price: 3, quantity: 2))) == [
      "2 x tea @ 3.00 = 6.00",
      "total: 6.00"
    ]
  end

  var rows = cart.lines.map(fn(line)
    "#{line.quantity} x #{line.name} @ #{money(line.price)} = #{money(line_total(line))}"
  end)
  case cart.code
    None: Unit
    Some(code): rows = rows.append("discount #{code_text(code)}: -#{money(discount(cart))}")
  end
  rows.append("total: #{money(total(cart))}")
end
