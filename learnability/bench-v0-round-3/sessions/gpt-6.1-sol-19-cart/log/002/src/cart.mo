## A shopping cart: its lines, its total and its receipt.
module Cart
expose Cart, Line(..), empty, add, subtotal, total, receipt

struct Line
  name: Text
  price: Decimal
  quantity: Int
end

struct Cart
  lines: List(Line)
end

fn empty() : Cart
  Cart(lines: [])
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
  Cart(lines: lines)
end

fn line_total(line: Line) : Decimal
  line.price * line.quantity.to_decimal
end

fn subtotal(cart: Cart) : Decimal
  cart.lines.reduce(0.0, fn(sum, line) sum + line_total(line) end)
end

fn total(cart: Cart) : Decimal
  subtotal(cart)
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
  rows.append("total: #{money(total(cart))}")
end
