## A module with every kind of declaration (reference 4, 5, 9, 10 and 12).
module Shop.OrderBook
expose Order, OrderError(..), Tree(..), place, total, max_items
use Shop.Catalog
use Shop.Money{Money, add, Currency}

max_items = 100

struct Order
  id: Int
  lines: List(OrderLine)
  paid?: Bool
  handler: fn(Order) : Result(Unit, OrderError)
  callback: fn(Text)
  thunk: fn() : Int
  pairs: List((Text, Map(Text, List(Int))))
  price: Catalog.Price
end

struct Box(T)
  value: T
end

enum Tree(T)
  Leaf
  Node(left: Tree(T), value: T, right: Tree(T))
end

enum OrderError
  Empty
  Invalid(Text)
  TooMany(count: Int, limit: Int,)
end

fn total(order: Order) : Decimal
  order.lines.map(fn(line) line.price end).sum
end

fn place(lines: List(OrderLine)) : Result(Order, OrderError)
  promise "an order is never empty"
    result is Ok(order) implies not order.lines.empty?
    allows lines: [one], result: Ok(order_of([one]))
    forbids lines: [], result: Ok(order_of([]))
  end

  promise "an order never has more lines than allowed"
    result is Ok(order) implies order.lines.size <= max_items
    forbids lines: [], result: Ok(order_of(many))
    allows lines: [], result: Error(Empty)
  end

  examples
    place([]) == Error(Empty)
    place([one]) is Ok(_)
  end

  return Error(Empty) if lines.empty?
  Ok(order_of(lines))
end

fn largest(items: List(T), key: fn(T) : K) : Option(T) where K: Order, T: Text
  items.sort_by(key).last
end

fn nothing()
end

test "an empty order is refused"
  assert place([]) == Error(Empty)
end
