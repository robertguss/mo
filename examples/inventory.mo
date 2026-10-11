## Generic helpers for lists, and a text table whose columns line up, used on
## a product list: the most valuable stock, the products in each category, and
## the categories once each.
module Inventory

struct Product
  name: Text
  category: Text
  price: Decimal
  stock: Int
end

## A table column: its title, how to show a row in it, and whether its cells
## line up on the right, as numbers do.
struct Column(T)
  title: Text
  cell: fn(T) : Text
  numeric?: Bool
end

## The n items with the largest keys, largest first. Items with equal keys keep
## their order.
fn top_by(items: List(T), n: Int, key: fn(T) : K) : List(T) where K: Order
  examples
    top_by([3, 1, 4, 1, 5], 2, fn(x) x end) == [5, 4]
    top_by(["bb", "a", "ccc"], 1, fn(s) s.char_count end) == ["ccc"]
    top_by([1, 2], 5, fn(x) x end) == [2, 1]
  end

  items.sort_by_desc(key).take(n)
end

## The items grouped by key: keys in the order they first appear, and each
## group in the items' order.
fn group_by_key(items: List(T), key: fn(T) : K) : Map(K, List(T)) where K: Hash
  examples
    group_by_key(["apple", "bean", "avocado"], fn(s) s.chars.first end) == {Some('a'): ["apple", "avocado"], Some('b'): ["bean"]}
  end

  var groups = {}
  for item in items
    groups = groups.update(key(item), [], fn(group) group.append(item) end)
  end
  groups
end

## Each item once, where it first appears.
fn distinct(items: List(T)) : List(T) where T: Hash
  examples
    distinct([2, 1, 2, 3, 1]) == [2, 1, 3]
    distinct(["a"]) == ["a"]
  end

  var seen = Set.empty
  var kept = []
  for item in items
    continue if seen.has?(item)
    seen = seen.add(item)
    kept = kept.append(item)
  end
  kept
end

## The rows laid out under the columns' titles, each column as wide as its
## widest cell, with two spaces between columns.
fn table(rows: List(T), columns: List(Column(T))) : Text
  examples
    table([1, 22], [Column(title: "n", cell: fn(n) "#{n}" end, numeric?: true)]) == " n\n 1\n22"
  end

  laid_out = columns.map(fn(column) pad_column(rows, column) end)
  lines = (0..rows.size + 1).to_list.map(fn(i)
    laid_out.map(fn(cells) cells.get(i) or "" end).join("  ").trim_end
  end)
  lines.join("\n")
end

## A column's title and cells, padded to the same width.
fn pad_column(rows: List(T), column: Column(T)) : List(Text)
  cells = [column.title].concat(rows.map(column.cell))
  width = cells.map(fn(cell) cell.char_count end).max or 0
  cells.map(fn(cell) if column.numeric?: cell.pad_left(width) else: cell.pad_right(width) end)
end

products = [
  Product(name: "Kettle", category: "kitchen", price: 39.90, stock: 12),
  Product(name: "Desk lamp", category: "office", price: 24.50, stock: 40),
  Product(name: "Chef's knife", category: "kitchen", price: 89.00, stock: 5),
  Product(name: "Notebook", category: "office", price: 3.20, stock: 300),
  Product(name: "Doormat", category: "home", price: 19.99, stock: 0)
]

product_columns = [
  Column(title: "product", cell: fn(p) p.name end, numeric?: false),
  Column(title: "category", cell: fn(p) p.category end, numeric?: false),
  Column(title: "price", cell: fn(p) p.price.to_fixed(2) end, numeric?: true),
  Column(title: "stock", cell: fn(p) "#{p.stock}" end, numeric?: true)
]

fn stock_value(product: Product) : Decimal
  product.price * product.stock.to_decimal
end

fn main(console: Console)
  console.print("The three most valuable stocks:")
  console.print(table(top_by(products, 3, stock_value), product_columns))
  console.print("")
  for (category, group) in group_by_key(products, fn(p) p.category end)
    names = group.map(fn(p) p.name end).join(", ")
    console.print("#{category}: #{names}")
  end
  console.print("")
  console.print("Categories: #{distinct(products.map(fn(p) p.category end)).join(", ")}")
end

test "the table lines up text on the left and numbers on the right"
  rows = [products.get(0), products.get(3)].filter_map(fn(p) p end)
  expected = """
    product   category  price  stock
    Kettle    kitchen   39.90     12
    Notebook  office     3.20    300
    """
  assert table(rows, product_columns) == expected
end
