## Tests of the Restock module.
module RestockTest
use Restock{Item, RowError}

test "a row is a SKU and three counts"
  assert Restock.parse_row("A1,3,5,20") == Ok(Item(sku: "A1", on_hand: 3, reorder_at: 5, order_up_to: 20))
  assert Restock.parse_row(" B-7 , 0 , 0 , 1 ") == Ok(Item(sku: "B-7", on_hand: 0, reorder_at: 0, order_up_to: 1))
end

test "a row with the wrong number of fields is refused"
  assert Restock.parse_row("A1,3,5") == Error(WrongFields(found: 3))
  assert Restock.parse_row("A1,3,5,20,1") == Error(WrongFields(found: 5))
  assert Restock.parse_row("") == Error(WrongFields(found: 1))
end

test "counts are whole numbers of at least 0"
  assert Restock.parse_row("A1,x,5,20") == Error(BadNumber(field: "x"))
  assert Restock.parse_row("A1,3,-5,20") == Error(BadNumber(field: "-5"))
  assert Restock.parse_row("A1,3,5,2.5") == Error(BadNumber(field: "2.5"))
end

test "the reorder level is below the level to order up to, and the SKU isn't empty"
  assert Restock.parse_row("A1,3,20,5") == Error(BadLevels(sku: "A1"))
  assert Restock.parse_row("A1,3,5,5") == Error(BadLevels(sku: "A1"))
  assert Restock.parse_row(" ,3,5,20") == Error(BadLevels(sku: ""))
end

test "an item is ordered up to its level once it is at or below its reorder level"
  assert Restock.order(Item(sku: "A1", on_hand: 3, reorder_at: 5, order_up_to: 20)) == 17
  assert Restock.order(Item(sku: "A1", on_hand: 5, reorder_at: 5, order_up_to: 20)) == 15
  assert Restock.order(Item(sku: "A1", on_hand: 6, reorder_at: 5, order_up_to: 20)) == 0
  assert Restock.order(Item(sku: "A1", on_hand: 0, reorder_at: 0, order_up_to: 1)) == 1
end

test "the plan lists the orders, then counts them"
  items = [
    Item(sku: "A1", on_hand: 3, reorder_at: 5, order_up_to: 20),
    Item(sku: "B2", on_hand: 9, reorder_at: 5, order_up_to: 20),
    Item(sku: "C3", on_hand: 0, reorder_at: 2, order_up_to: 4)
  ]
  assert Restock.plan(items) == ["A1: order 17", "C3: order 4", "2 items to order"]
  assert Restock.plan(items.take(1)) == ["A1: order 17", "1 item to order"]
  assert Restock.plan([]) == ["0 items to order"]
end

test "main reads the file named on the command line"
  console = Console.fake()
  files = Files.in_memory({"stock.csv": "sku,on_hand,reorder_at,order_up_to\nA1,3,5,20\nB2,x,1,2\n\nC3,0,2,4\n"}).read_only
  assert Restock.main(Args.of(["stock.csv"]), console, files) == 1
  assert console.printed == ["A1: order 17", "C3: order 4", "2 items to order"]
  assert console.printed_errors == ["line 3: not a count: x"]
end

test "main needs one file it can read"
  console = Console.fake()
  files = Files.in_memory({}).read_only
  assert Restock.main(Args.of(["gone.csv"]), console, files) == 2
  assert console.printed_errors == ["restock: gone.csv: no such file"]
  other = Console.fake()
  assert Restock.main(Args.of([]), other, files) == 2
  assert other.printed_errors == ["usage: restock FILE"]
end
