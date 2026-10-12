## Inventory: generic list helpers (the top N by a key, grouping by a key,
## removing duplicates) and text tables with aligned columns, used on a stock
## list of products.
##
## Usage: inventory [N]. N, a whole number from 1 up, is how many products
## each top list shows, and is 3 when not given.
module Inventory
expose top_by, group_by_key, distinct_by, Align(..), Column(..), table
use Unicode16

## How a column lines up its cells.
enum Align
  Left
  Right
end

## A column of a table: its heading, how it lines up its cells, and how a row
## gives its cell.
struct Column(T)
  heading: Text
  align: Align
  cell: fn(T) : Text
end

## A product line in the stock list.
struct Product
  sku: Text
  name: Text
  category: Text
  price: Decimal
  quantity: Int
end

## The stock of a group of products, summed up.
struct Summary
  label: Text
  product_count: Int
  units: Int
  value: Decimal
  most_valuable: Text
end

# Generic helpers

## The n items with the largest keys, largest first. Items with equal keys
## keep their order in the list, and an n of 0 or less gives no items.
fn top_by(items: List(T), n: Int, key: fn(T) : K) : List(T) where K: Order
  examples
    top_by([3, 1, 4, 1, 5], 3, fn(v) v end) == [5, 4, 3]
    top_by(["bb", "a", "ccc"], 2, fn(w) w.char_count end) == ["ccc", "bb"]
    top_by(["b", "a", "c"], 2, fn(_letter) 0 end) == ["b", "a"]
    top_by([1, 2], 5, fn(v) v end) == [2, 1]
    top_by([1, 2], 0, fn(v) v end) == []
  end

  items.sort_by_desc(key).take(n.max(0))
end

## Splits items into groups that share a key. The groups come in the order
## their keys first appear, and each keeps its items in list order.
fn group_by_key(items: List(T), key: fn(T) : K) : Map(K, List(T)) where K: Hash
  examples
    group_by_key([1, 2, 3, 4, 5], fn(v) v % 2 end) == {1: [1, 3, 5], 0: [2, 4]}
    group_by_key(["kiwi", "fig", "plum"], fn(w) w.char_count end).keys == [4, 3]
  end

  var groups = {}
  for item in items
    groups = groups.update(key(item), [], fn(members) members.append(item) end)
  end
  groups
end

## Removes duplicates: each item whose key an earlier item already has. The
## first item with each key stays, and the items keep their order.
fn distinct_by(items: List(T), key: fn(T) : K) : List(T) where K: Hash
  examples
    distinct_by([3, 1, 3, 2, 1], fn(v) v end) == [3, 1, 2]
    distinct_by(["apple", "Apple", "pear"], fn(w) w.ascii_lowercase end) == ["apple", "pear"]
  end

  var seen = Set.empty
  var kept = []
  for item in items
    item_key = key(item)
    continue if seen.has?(item_key)
    seen = seen.add(item_key)
    kept = kept.append(item)
  end
  kept
end

# Text tables

## Lays rows out as lines of text in aligned columns: a heading line, a rule
## under it, then a line for each row. Each column is as wide as its widest
## cell, counted in terminal columns, and columns are two spaces apart.
fn table(rows: List(T), columns: List(Column(T))) : List(Text)
  examples
    table([7, 42], [Column(heading: "n", align: Right, cell: fn(v) "#{v}" end)]) == [" n", "--", " 7", "42"]
  end

  headings = columns.map(fn(column) column.heading end)
  aligns = columns.map(fn(column) column.align end)
  body = rows.map(fn(row) columns.map(fn(column) column.cell(row) end) end)
  widths = column_widths(body.prepend(headings))
  rule = widths.map(fn(width) "-".repeat(width) end)
  body.prepend(rule).prepend(headings).map(fn(cells) table_line(cells, widths, aligns) end)
end

## The display width of the widest cell in each column of a grid. A line with
## fewer cells than another counts as empty in the columns it lacks.
fn column_widths(grid: List(List(Text))) : List(Int)
  promise "there is a width for each column, and every cell fits in its column"
    result.size == (grid.map(fn(cells) cells.size end).max or 0)
    grid.all?(fn(cells) cells.zip(result).all?(fn(pair) Unicode16.display_width(pair.0) <= pair.1 end) end)
    allows grid: [["ab", "c"]], result: [2, 1]
    forbids grid: [["ab", "c"]], result: [1, 1]
  end
  examples
    column_widths([["a", "bbb"], ["cc"]]) == [2, 3]
    column_widths([["急須", "pot"]]) == [4, 3]
    column_widths([]) == []
  end

  column_count = grid.map(fn(cells) cells.size end).max or 0
  positions = (0..column_count).to_list
  positions.map(fn(i)
    grid.map(fn(cells) Unicode16.display_width(cells.get(i) or "") end).max or 0
  end)
end

## One line of a table: each cell padded to its column's width and lined up
## as its column says, two spaces apart, with no spaces left at the end.
fn table_line(cells: List(Text), widths: List(Int), aligns: List(Align)) : Text
  examples
    table_line(["a", "1"], [3, 2], [Left, Right]) == "a     1"
    table_line(["1", "a"], [2, 3], [Right, Left]) == " 1  a"
  end

  var parts = []
  for ((cell, width), align) in cells.zip(widths).zip(aligns)
    parts = parts.append(pad(cell, width, align))
  end
  parts.join("  ").trim_end
end

## Pads a cell with spaces up to a display width: after it to line it up on
## the left, before it to line it up on the right. Widths count terminal
## columns, so a wide character such as 急 counts as two. A cell already as
## wide is left as it is.
fn pad(cell: Text, width: Int, align: Align) : Text
  examples
    pad("ab", 4, Left) == "ab  "
    pad("ab", 4, Right) == "  ab"
    pad("急須", 6, Left) == "急須  "
    pad("abcdef", 4, Right) == "abcdef"
  end

  gap = " ".repeat((width - Unicode16.display_width(cell)).max(0))
  case align
    Left: "#{cell}#{gap}"
    Right: "#{gap}#{cell}"
  end
end

# The stock list

## The stock list as two supplier feeds delivered it, one after the other.
## The second feed lists two products again, one of them with another count;
## the first listing of a product is the one that counts.
sample_products = [
  Product(sku: "K-100", name: "Chef knife", category: "Kitchen", price: 49.90, quantity: 12),
  Product(sku: "K-101", name: "Cutting board", category: "Kitchen", price: 18.50, quantity: 30),
  Product(sku: "G-200", name: "Pruning shears", category: "Garden", price: 22.75, quantity: 15),
  Product(sku: "T-300", name: "Claw hammer", category: "Tools", price: 16.90, quantity: 25),
  Product(sku: "T-301", name: "Cordless drill", category: "Tools", price: 89.00, quantity: 6),
  Product(sku: "G-201", name: "Watering can", category: "Garden", price: 12.40, quantity: 40),
  Product(sku: "K-102", name: "Teapot 急須", category: "Kitchen", price: 34.00, quantity: 7),
  Product(sku: "K-101", name: "Cutting board", category: "Kitchen", price: 18.50, quantity: 30),
  Product(sku: "G-202", name: "Gardening gloves", category: "Garden", price: 9.95, quantity: 60),
  Product(sku: "T-302", name: "Tape measure", category: "Tools", price: 7.25, quantity: 0),
  Product(sku: "T-301", name: "Cordless drill", category: "Tools", price: 89.00, quantity: 4),
  Product(sku: "K-103", name: "Crêpe pan", category: "Kitchen", price: 27.30, quantity: 9)
]

## What a product's stock is worth at its price.
fn stock_value(product: Product) : Decimal
  examples
    stock_value(Product(sku: "X-1", name: "Mug", category: "Kitchen", price: 4.50, quantity: 3)) == 13.50
  end

  product.price * product.quantity.to_decimal
end

## Sums up the stock of some products under a label, and names the product
## whose stock is worth the most, or "-" when there are none.
fn summarize(label: Text, products: List(Product)) : Summary
  examples
    summarize("Nothing", []) == Summary(label: "Nothing", product_count: 0, units: 0, value: 0, most_valuable: "-")
  end

  leader = top_by(products, 1, stock_value).first.map(fn(top) top.name end) or "-"
  Summary(
    label: label,
    product_count: products.size,
    units: products.map(fn(item) item.quantity end).sum,
    value: products.map(stock_value).sum,
    most_valuable: leader
  )
end

## Sums up each category's stock, in the order the categories first appear.
fn category_totals(products: List(Product)) : List(Summary)
  group_by_key(products, fn(product) product.category end)
    .entries
    .map(fn(entry) summarize(entry.0, entry.1) end)
end

## The columns of the stock list.
fn product_columns() : List(Column(Product))
  [
    Column(heading: "SKU", align: Left, cell: fn(product) product.sku end),
    Column(heading: "Product", align: Left, cell: fn(product) product.name end),
    Column(heading: "Category", align: Left, cell: fn(product) product.category end),
    Column(heading: "Price", align: Right, cell: fn(product) product.price.to_fixed(2) end),
    Column(heading: "Units", align: Right, cell: fn(product) "#{product.quantity}" end),
    Column(heading: "Value", align: Right, cell: fn(product) stock_value(product).to_fixed(2) end)
  ]
end

## The columns of the category summary.
fn summary_columns() : List(Column(Summary))
  [
    Column(heading: "Category", align: Left, cell: fn(summary) summary.label end),
    Column(heading: "Products", align: Right, cell: fn(summary) "#{summary.product_count}" end),
    Column(heading: "Units", align: Right, cell: fn(summary) "#{summary.units}" end),
    Column(heading: "Value", align: Right, cell: fn(summary) summary.value.to_fixed(2) end),
    Column(heading: "Most valuable", align: Left, cell: fn(summary) summary.most_valuable end)
  ]
end

# The program

## How many products each top list shows: the one argument, a whole number
## from 1 up, or 3 when there is no argument. None for anything else.
fn top_count(arguments: List(Text)) : Option(Int)
  promise "a count it gives is at least 1"
    result is Some(given) implies given >= 1
    allows arguments: ["4"], result: Some(4)
    forbids arguments: ["0"], result: Some(0)
  end
  examples
    top_count([]) == Some(3)
    top_count(["5"]) == Some(5)
    top_count(["0"]) == None
    top_count(["five"]) == None
    top_count(["2", "4"]) == None
  end

  return Some(3) if arguments.empty?
  return None if arguments.size > 1
  text = try arguments.first
  n = try text.to_int
  if n >= 1: Some(n) else: None
end

## Prints a title, the lines under it, and a blank line.
fn print_section(console: Console, title: Text, lines: List(Text))
  console.print(title)
  for line in lines
    console.print(line)
  end
  console.print("")
end

## Prints the stock list without its duplicate lines, the products whose stock
## is worth the most, those with the fewest units, and a summary of each
## category.
fn main(args: Args, console: Console) : Int
  shown = case top_count(args.list)
    Some(n): n
    None:
      console.error("usage: inventory [N], where N, a whole number from 1 up, is how many products each top list shows (3 when not given)")
      return 2
  end
  products = distinct_by(sample_products, fn(product) product.sku end)
  removed = sample_products.size - products.size
  noun = if removed == 1: "line" else: "lines"
  columns = product_columns()
  print_section(console, "Stock list: #{products.size} products, #{removed} duplicate #{noun} removed", table(products, columns))
  print_section(console, "Top #{shown} by stock value", table(top_by(products, shown, stock_value), columns))
  print_section(console, "Top #{shown} by fewest units", table(top_by(products, shown, fn(product) -product.quantity end), columns))
  totals = category_totals(products).append(summarize("All", products))
  print_section(console, "By category", table(totals, summary_columns()))
  0
end

test "a table pads each column to its widest cell"
  drinks = [("tea", 3), ("coffee", 12)]
  rendered = table(drinks, [
    Column(heading: "drink", align: Left, cell: fn(drink) drink.0 end),
    Column(heading: "cups", align: Right, cell: fn(drink) "#{drink.1}" end)
  ])
  assert rendered == ["drink   cups", "------  ----", "tea        3", "coffee    12"]
end

test "duplicate lines are dropped by SKU, keeping the first of each"
  products = distinct_by(sample_products, fn(product) product.sku end)
  assert products.size == 10
  assert products.find(fn(product) product.sku == "T-301" end).map(fn(drill) drill.quantity end) == Some(6)
end

test "the top lists put the largest key first"
  products = distinct_by(sample_products, fn(product) product.sku end)
  richest = top_by(products, 3, stock_value)
  assert richest.map(fn(product) product.sku end) == ["K-100", "G-202", "K-101"]
  fewest = top_by(products, 3, fn(product) -product.quantity end)
  assert fewest.map(fn(product) product.sku end) == ["T-302", "T-301", "K-102"]
end

test "categories are summed up in the order they first appear"
  totals = category_totals(distinct_by(sample_products, fn(product) product.sku end))
  assert totals.map(fn(total) total.label end) == ["Kitchen", "Garden", "Tools"]
  assert totals.map(fn(total) total.units end) == [58, 115, 31]
  assert totals.map(fn(total) total.value end) == [1637.50, 1434.25, 956.50]
  assert totals.map(fn(total) total.most_valuable end) == ["Chef knife", "Gardening gloves", "Cordless drill"]
end

test "the report opens with the stock list and shows as many top products as asked"
  console = Console.fake()
  assert main(Args.of(["2"]), console) == 0
  assert console.printed.first == Some("Stock list: 10 products, 2 duplicate lines removed")
  assert console.printed.contains?("Top 2 by stock value")
  assert console.printed_errors.empty?
end

test "a top count that is not a whole number from 1 up is refused"
  console = Console.fake()
  assert main(Args.of(["0"]), console) == 2
  assert console.printed.empty?
  assert console.printed_errors.size == 1
end
