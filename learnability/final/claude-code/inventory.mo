## Inventory: generic list helpers (the top N by a key, grouping by a key,
## removing duplicates) and a text table with aligned columns, used on a
## list of products to print a stock report.
module Inventory
expose top_by, group_by_key, unique_by, dedupe, repeated_keys, Align(..), Column(..), Table(..), render
use Unicode16

# ---------------------------------------------------------------------------
# Generic helpers
# ---------------------------------------------------------------------------

## The `n` items with the largest keys, largest first. Items with equal keys
## keep their order in the list, and a list shorter than `n` comes back
## whole, sorted. For the smallest keys, negate the key.
fn top_by(items: List(T), n: Int, key: fn(T) : K) : List(T) where K: Order
  examples
    top_by([3, 1, 4, 1, 5], 3, fn(x) x end) == [5, 4, 3]
    top_by(["fig", "apple", "kiwi", "pear"], 2, fn(word) word.char_count end) == ["apple", "kiwi"]
    top_by([3, 1, 4], 10, fn(x) -x end) == [1, 3, 4]
    top_by([3, 1, 4], 0, fn(x) x end) == []
  end

  items.sort_by_desc(key).take(n.max(0))
end

## The items in groups that share a key. Keys come in the order they first
## appear, and each group keeps its items in the order of the list.
fn group_by_key(items: List(T), key: fn(T) : K) : Map(K, List(T)) where K: Hash
  examples
    group_by_key([1, 2, 3, 4, 5], fn(x) x % 2 == 0 end) == {false: [1, 3, 5], true: [2, 4]}
    group_by_key(["pear", "fig", "plum"], fn(word) word.char_count end).keys == [4, 3]
  end

  var groups = {}
  for item in items
    groups = groups.update(key(item), [], fn(group) group.append(item) end)
  end
  groups
end

## The items without repeats: of the items that share a key, only the first
## is kept, and the kept items stay in the order of the list.
fn unique_by(items: List(T), key: fn(T) : K) : List(T) where K: Hash
  examples
    unique_by(["apple", "avocado", "banana", "cherry", "blueberry"], fn(word) word.chars.first end) == ["apple", "banana", "cherry"]
    unique_by([5, 15, 7, 25], fn(x) x % 10 end) == [5, 7]
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

## The items without repeated values, each kept where it first appears.
fn dedupe(items: List(T)) : List(T) where T: Hash
  examples
    dedupe([3, 1, 3, 2, 1]) == [3, 1, 2]
    dedupe(["b", "a", "b"]) == ["b", "a"]
  end

  unique_by(items, fn(item) item end)
end

## The keys that more than one item has, in the order they first appear:
## what `unique_by` would drop items for.
fn repeated_keys(items: List(T), key: fn(T) : K) : List(K) where K: Hash
  examples
    repeated_keys([1, 2, 3, 1, 1, 2], fn(x) x end) == [1, 2]
    repeated_keys(["one", "two", "three"], fn(word) word.char_count end) == [3]
  end

  group_by_key(items, key).entries.filter_map(fn(entry)
    (item_key, group) = entry
    if group.size > 1: Some(item_key) else: None
  end)
end

# ---------------------------------------------------------------------------
# Text tables
# ---------------------------------------------------------------------------

## The side of its column a cell lines up on: text usually on the left,
## numbers on the right.
enum Align
  Left
  Right
end

## A column of a table: its heading, and the side its cells line up on.
struct Column
  heading: Text
  align: Align
end

## A table of text cells. Each row, and each row of the footer, holds a cell
## for each column: a missing cell counts as empty, and cells past the last
## column are left out. The footer, such as a row of totals, is set off from
## the rows by a rule.
struct Table
  columns: List(Column)
  rows: List(List(Text))
  footer: List(List(Text))
end

## Columns are this far apart.
column_gap = "  "

## The table as lines of text: the headings, a rule of dashes under them, the
## rows, and then, when there is a footer, another rule and the footer. Every
## cell is padded to its column's width, so the columns line up, and no line
## ends in spaces.
fn render(table: Table) : List(Text)
  widths = column_widths(table)
  line_of = fn(cells) render_row(table.columns, widths, cells) end
  rule = widths.map(fn(width) "-".repeat(width) end).join(column_gap)
  headings = table.columns.map(fn(column) column.heading end)
  lines = [line_of(headings), rule].concat(table.rows.map(line_of))
  if table.footer.empty?
    lines
  else
    lines.append(rule).concat(table.footer.map(line_of))
  end
end

## How wide each column must be to hold its heading and every one of its
## cells, in the rows and in the footer.
fn column_widths(table: Table) : List(Int)
  all_rows = table.rows.concat(table.footer)
  table.columns.enumerate.map(fn(entry)
    (i, column) = entry
    all_rows.reduce(screen_width(column.heading), fn(widest, row)
      widest.max(screen_width(row.get(i) or ""))
    end)
  end)
end

## One line of a table: each cell padded to its column's width, on the side
## its column says, with the columns `column_gap` apart.
fn render_row(columns: List(Column), widths: List(Int), cells: List(Text)) : Text
  padded = columns.enumerate.map(fn(entry)
    (i, column) = entry
    pad_cell(cells.get(i) or "", widths.get(i) or 0, column.align)
  end)
  padded.join(column_gap).trim_end
end

## The cell with spaces added to make it `width` wide: after it when it lines
## up on the left, before it when it lines up on the right. A cell that is
## already as wide, or wider, stays as it is.
fn pad_cell(cell: Text, width: Int, align: Align) : Text
  examples
    pad_cell("ab", 4, Left) == "ab  "
    pad_cell("ab", 4, Right) == "  ab"
    pad_cell("abcdef", 4, Right) == "abcdef"
  end

  fill = " ".repeat((width - screen_width(cell)).max(0))
  case align
    Left: "#{cell}#{fill}"
    Right: "#{fill}#{cell}"
  end
end

## How many columns of the screen a text takes. Wide characters, as in
## Chinese or Japanese, take two, so measuring this way rather than counting
## characters keeps such columns lined up.
fn screen_width(text: Text) : Int
  Unicode16.display_width(text)
end

# ---------------------------------------------------------------------------
# Products
# ---------------------------------------------------------------------------

## A product held in stock.
struct Product
  sku: Text         # the stock-keeping code, one for each product
  name: Text
  category: Text
  price: Decimal    # for one unit
  quantity: Int     # units in stock
end

## What a product's units in stock are worth at its price.
fn stock_value(product: Product) : Decimal
  product.price * product.quantity.to_decimal
end

## An amount of money with two decimal places, as in "1398.60".
fn money(amount: Decimal) : Text
  examples
    money(89) == "89.00"
    money(1398.6) == "1398.60"
  end

  amount.to_fixed(2)
end

## A count and a noun that agrees with it: "1 product", "9 products".
fn counted(n: Int, one: Text, many: Text) : Text
  examples
    counted(1, "product", "products") == "1 product"
    counted(5, "category", "categories") == "5 categories"
  end

  if n == 1: "1 #{one}" else: "#{n} #{many}"
end

## A table with a row for each product: its code, name and category, its
## price, its units in stock, and what they are worth.
fn product_table(products: List(Product)) : Table
  Table(
    columns: [
      Column(heading: "SKU", align: Left),
      Column(heading: "Product", align: Left),
      Column(heading: "Category", align: Left),
      Column(heading: "Price", align: Right),
      Column(heading: "Units", align: Right),
      Column(heading: "Value", align: Right)
    ],
    rows: products.map(product_row),
    footer: []
  )
end

## A product's cells in a product table.
fn product_row(product: Product) : List(Text)
  [
    product.sku,
    product.name,
    product.category,
    money(product.price),
    product.quantity.to_text,
    money(stock_value(product))
  ]
end

## A table with a row for each category, in the order categories first
## appear: how many products it has, their units in stock, what those are
## worth, and its most valuable product. The footer sums up every product.
fn category_table(products: List(Product)) : Table
  groups = group_by_key(products, fn(product) product.category end)
  rows = groups.entries.map(fn(entry)
    (category, members) = entry
    summary_row(category, members)
  end)
  Table(
    columns: [
      Column(heading: "Category", align: Left),
      Column(heading: "Products", align: Right),
      Column(heading: "Units", align: Right),
      Column(heading: "Value", align: Right),
      Column(heading: "Most valuable", align: Left)
    ],
    rows: rows,
    footer: [summary_row("Total", products)]
  )
end

## Some products summed up under a label: how many there are, their units in
## stock, what those are worth, and the name of the most valuable of them.
fn summary_row(label: Text, products: List(Product)) : List(Text)
  units = products.map(fn(product) product.quantity end).sum
  value = products.map(stock_value).sum
  best = top_by(products, 1, stock_value).first.map(fn(product) product.name end)
  [label, products.size.to_text, units.to_text, money(value), best or ""]
end

## The stock lists of two stores, merged: two products are on both, so they
## are listed twice.
fn sample_products() : List(Product)
  [
    Product(sku: "LMP-01", name: "Desk lamp", category: "Lighting", price: 24.99, quantity: 12),
    Product(sku: "LMP-02", name: "Floor lamp", category: "Lighting", price: 89.00, quantity: 3),
    Product(sku: "CHR-01", name: "Office chair", category: "Furniture", price: 149.50, quantity: 7),
    Product(sku: "DSK-01", name: "Standing desk", category: "Furniture", price: 399.00, quantity: 2),
    Product(sku: "CBL-01", name: "USB-C cable", category: "Cables", price: 9.99, quantity: 140),
    Product(sku: "CBL-02", name: "HDMI cable", category: "Cables", price: 14.50, quantity: 65),
    Product(sku: "LMP-01", name: "Desk lamp", category: "Lighting", price: 24.99, quantity: 12),
    Product(sku: "MUG-01", name: "Café mug", category: "Kitchen", price: 7.25, quantity: 48),
    Product(sku: "KTL-01", name: "Electric kettle", category: "Kitchen", price: 34.90, quantity: 9),
    Product(sku: "CBL-01", name: "USB-C cable", category: "Cables", price: 9.99, quantity: 140),
    Product(sku: "BAG-01", name: "Laptop bag", category: "Accessories", price: 59.00, quantity: 15)
  ]
end

# ---------------------------------------------------------------------------
# The report
# ---------------------------------------------------------------------------

## Prints a stock report: a summary, then tables of every product, the most
## valuable stock, the lowest stock, and the stock in each category.
fn main(console: Console)
  listed = sample_products()
  stock = unique_by(listed, fn(product) product.sku end)
  categories = dedupe(stock.map(fn(product) product.category end))
  repeated = repeated_keys(listed, fn(product) product.sku end)

  products_text = counted(stock.size, "product", "products")
  categories_text = counted(categories.size, "category", "categories")
  repeated_text = if repeated.empty?: "none" else: repeated.join(", ")
  dropped_text = counted(listed.size - stock.size, "line", "lines")
  console.print("Inventory: #{products_text} in #{categories_text}")
  console.print("Categories: #{categories.join(", ")}")
  console.print("Listed more than once: #{repeated_text} (#{dropped_text} dropped)")
  console.print("")

  show(console, "All products", product_table(stock))
  show(console, "Top 3 by stock value", product_table(top_by(stock, 3, stock_value)))
  # With the quantity negated, the fewest units make the largest key.
  show(console, "Lowest stock", product_table(top_by(stock, 3, fn(product) -product.quantity end)))
  show(console, "By category", category_table(stock))
end

## Prints a title, the table under it, and a blank line.
fn show(console: Console, title: Text, table: Table)
  console.print(title)
  for line in render(table)
    console.print(line)
  end
  console.print("")
end

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

test "columns line up, with text on the left and numbers on the right"
  table = Table(
    columns: [Column(heading: "Name", align: Left), Column(heading: "Qty", align: Right)],
    rows: [["Pen", "5"], ["Notebook", "120"]],
    footer: [["Total", "125"]]
  )
  assert render(table) == [
    "Name      Qty",
    "--------  ---",
    "Pen         5",
    "Notebook  120",
    "--------  ---",
    "Total     125"
  ]
end

test "wide characters take two columns, and a missing cell counts as empty"
  table = Table(
    columns: [Column(heading: "Item", align: Left), Column(heading: "Units", align: Right)],
    rows: [["お茶", "3"], ["Café"]],
    footer: []
  )
  assert render(table) == [
    "Item  Units",
    "----  -----",
    "お茶      3",
    "Café"
  ]
end

test "a product listed twice is kept once, where it first appears"
  listed = sample_products()
  stock = unique_by(listed, fn(product) product.sku end)
  assert stock.map(fn(product) product.sku end) == ["LMP-01", "LMP-02", "CHR-01", "DSK-01", "CBL-01", "CBL-02", "MUG-01", "KTL-01", "BAG-01"]
  assert repeated_keys(listed, fn(product) product.sku end) == ["LMP-01", "CBL-01"]
end

test "the most valuable stock comes first, and negated units give the lowest stock"
  stock = unique_by(sample_products(), fn(product) product.sku end)
  assert render(product_table(top_by(stock, 3, stock_value))) == [
    "SKU     Product       Category    Price  Units    Value",
    "------  ------------  ---------  ------  -----  -------",
    "CBL-01  USB-C cable   Cables       9.99    140  1398.60",
    "CHR-01  Office chair  Furniture  149.50      7  1046.50",
    "CBL-02  HDMI cable    Cables      14.50     65   942.50"
  ]
  lowest = top_by(stock, 3, fn(product) -product.quantity end)
  assert lowest.map(fn(product) product.sku end) == ["DSK-01", "LMP-02", "CHR-01"]
end

test "each category is summed up in the order it first appears, with totals below"
  stock = unique_by(sample_products(), fn(product) product.sku end)
  assert render(category_table(stock)) == [
    "Category     Products  Units    Value  Most valuable",
    "-----------  --------  -----  -------  -------------",
    "Lighting            2     15   566.88  Desk lamp",
    "Furniture           2      9  1844.50  Office chair",
    "Cables              2    205  2341.10  USB-C cable",
    "Kitchen             2     57   662.10  Café mug",
    "Accessories         1     15   885.00  Laptop bag",
    "-----------  --------  -----  -------  -------------",
    "Total               9    301  6299.58  USB-C cable"
  ]
end

test "the report opens with a summary of the stock"
  console = Console.fake()
  main(console)
  assert console.printed.take(3) == [
    "Inventory: 9 products in 5 categories",
    "Categories: Lighting, Furniture, Cables, Kitchen, Accessories",
    "Listed more than once: LMP-01, CBL-01 (2 lines dropped)"
  ]
  assert console.printed.contains?("By category")
end
