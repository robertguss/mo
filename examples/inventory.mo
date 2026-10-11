## Generic helpers for lists, and a text table whose columns line up, used on
## a product list: the most valuable stock, how many products each category
## has, and one product from each category.
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

## How many items have each key, keys in the order they first appear.
fn count_by(items: List(T), key: fn(T) : K) : Map(K, Int) where K: Hash
  examples
    count_by(["apple", "bean", "avocado"], fn(s) s.char_count end) == {5: 1, 4: 1, 7: 1}
    count_by([1, 2, 3, 4], fn(n) n % 2 end) == {1: 2, 0: 2}
  end

  var counts = {}
  for item in items
    counts = counts.update(key(item), 0, fn(n) n + 1 end)
  end
  counts
end

## The first item for each key, in the items' order.
fn first_by(items: List(T), key: fn(T) : K) : List(T) where K: Hash
  examples
    first_by(["apple", "bean", "avocado"], fn(s) s.chars.first end) == ["apple", "bean"]
    first_by([3, 3, 1], fn(n) n end) == [3, 1]
  end

  var seen = Set.empty
  var kept = []
  for item in items
    k = key(item)
    continue if seen.has?(k)
    seen = seen.add(k)
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

kettle = Product(name: "Kettle", category: "kitchen", price: 39.90, stock: 12)
lamp = Product(name: "Desk lamp", category: "office", price: 24.50, stock: 40)
knife = Product(name: "Chef's knife", category: "kitchen", price: 89.00, stock: 5)
notebook = Product(name: "Notebook", category: "office", price: 3.20, stock: 300)
doormat = Product(name: "Doormat", category: "home", price: 19.99, stock: 0)
products = [kettle, lamp, knife, notebook, doormat]

fn product_columns() : List(Column(Product))
  [
    Column(title: "product", cell: fn(p) p.name end, numeric?: false),
    Column(title: "category", cell: fn(p) p.category end, numeric?: false),
    Column(title: "price", cell: fn(p) p.price.to_fixed(2) end, numeric?: true),
    Column(title: "stock", cell: fn(p) "#{p.stock}" end, numeric?: true)
  ]
end

fn stock_value(product: Product) : Decimal
  product.price * product.stock.to_decimal
end

fn main(console: Console)
  console.print("The three most valuable stocks:")
  console.print(table(top_by(products, 3, stock_value), product_columns()))
  console.print("")
  for (category, count) in count_by(products, fn(p) p.category end)
    console.print("#{category}: #{count}")
  end
  console.print("")
  picks = first_by(products, fn(p) p.category end).map(fn(p) p.name end)
  console.print("One from each category: #{picks.join(", ")}")
end

test "the table lines up text on the left and numbers on the right"
  expected = """
    product   category  price  stock
    Kettle    kitchen   39.90     12
    Notebook  office     3.20    300
    """
  assert table([kettle, notebook], product_columns()) == expected
end

test "the most valuable stock comes first"
  assert top_by(products, 2, stock_value) == [lamp, notebook]
end
