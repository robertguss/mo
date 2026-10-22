## Reads a stock file and prints how much of each item to order.
module Restock
expose Item(..), RowError(..), parse_row, order, plan, main

struct Item
  sku: Text
  on_hand: Int
  reorder_at: Int
  order_up_to: Int
end

enum RowError
  WrongFields(found: Int)
  BadNumber(field: Text)
  BadLevels(sku: Text)
end

fn count(text: Text) : Result(Int, RowError)
  n = try text.trim.to_int.or_fail(BadNumber(field: text.trim))
  return Error(BadNumber(field: text.trim)) if n < 0
  Ok(n)
end

## Reads a row `SKU,ON_HAND,REORDER_AT,ORDER_UP_TO`.
fn parse_row(line: Text) : Result(Item, RowError)
  examples
    parse_row("A1,3,5,20") == Ok(Item(sku: "A1", on_hand: 3, reorder_at: 5, order_up_to: 20))
    parse_row("A1,3,5") == Error(WrongFields(found: 3))
    parse_row("A1,x,5,20") == Error(BadNumber(field: "x"))
    parse_row("A1,3,20,5") == Error(BadLevels(sku: "A1"))
  end

  fields = line.split(",")
  case fields
    [sku, on_hand, reorder_at, order_up_to]:
      item = Item(
        sku: sku.trim,
        on_hand: try count(on_hand),
        reorder_at: try count(reorder_at),
        order_up_to: try count(order_up_to)
      )
      return Error(BadLevels(sku: item.sku)) if item.sku.empty? or
        item.reorder_at >= item.order_up_to
      Ok(item)
    _: Error(WrongFields(found: fields.size))
  end
end

## How many to order: enough to reach `order_up_to`, once stock is at or below `reorder_at`.
fn order(item: Item) : Int
  examples
    order(Item(sku: "A1", on_hand: 3, reorder_at: 5, order_up_to: 20)) == 17
    order(Item(sku: "A1", on_hand: 5, reorder_at: 5, order_up_to: 20)) == 15
    order(Item(sku: "A1", on_hand: 6, reorder_at: 5, order_up_to: 20)) == 0
  end

  if item.on_hand < item.reorder_at: item.order_up_to - item.on_hand else: 0
end

## The order lines for the items that need any, then the count of them.
fn plan(items: List(Item)) : List(Text)
  examples
    plan([]) == ["0 items to order"]
  end

  wanted = items.filter(fn(item) order(item) > 0 end)
  lines = wanted.map(fn(item) "#{item.sku}: order #{order(item)}" end)
  noun = if wanted.size == 1: "item" else: "items"
  lines.append("#{wanted.size} #{noun} to order")
end

fn describe(error: RowError) : Text
  case error
    WrongFields(found: n): "expected 4 fields, found #{n}"
    BadNumber(field: f): "not a count: #{f}"
    BadLevels(sku: s): "bad levels for #{s}"
  end
end

fn main(args: Args, console: Console, files: ReadFiles) : Int
  path = case args.list
    [only]: only
    _:
      console.error("usage: restock FILE")
      return 2
  end
  text = case files.read_text(path)
    Ok(t): t
    Error(failure):
      console.error("restock: #{failure}")
      return 2
  end
  var items = []
  var status = 0
  for (index, line) in text.lines.enumerate
    continue if index == 0 or line.trim.empty?
    case parse_row(line)
      Ok(item): items = items.append(item)
      Error(error):
        console.error("line #{index + 1}: #{describe(error)}")
        status = 1
    end
  end
  for line in plan(items)
    console.print(line)
  end
  status
end
