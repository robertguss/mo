## Generic collection helpers and display-width-aligned inventory tables.
module Inventory
expose Product(..), top_n, group_items, deduplicate, table
use Unicode16

struct Product
  name: Text
  category: Text
  price: Decimal
  stock: Int
end

fn top_n(items: List(T), count: Int, key: fn(T) : K) : List(T) where K: Order
  items.sort_by_desc(key).take(count.max(0))
end

fn group_items(items: List(T), key: fn(T) : K) : Map(K, List(T)) where K: Hash
  items.group_by(key)
end

fn deduplicate(items: List(T)) : List(T) where T: Hash
  items.unique
end

fn one_line(cell: Text) : Text
  cell.replace("\n", " ").replace("\r", " ").replace("\t", " ")
end

fn render_row(row: List(Text), widths: List(Int)) : Text
  widths.enumerate.map(fn(pair)
    cell = one_line(row.get(pair.0) or "")
    padding = pair.1 - Unicode16.display_width(cell)
    "#{cell}#{" ".repeat(padding.max(0))}"
  end).join("  ").trim_end
end

fn table(headers: List(Text), rows: List(List(Text))) : Text
  all_rows = [headers].concat(rows)
  column_count = all_rows.map(fn(row) row.size end).max or 0
  return "" if column_count == 0
  var widths = (0..column_count).to_list.map(fn(_) 0 end)
  for row in all_rows
    for (index, cell) in row.enumerate
      previous = widths.get(index) or 0
      width = previous.max(Unicode16.display_width(one_line(cell)))
      widths = case widths.set(index, width)
        Some(updated): updated
        None: stop "all row columns fit the maximum column count"
      end
    end
  end
  var lines = [render_row(headers, widths), widths.map(fn(width) "-".repeat(width) end).join("  ")]
  for row in rows
    lines = lines.append(render_row(row, widths))
  end
  lines.join("\n")
end

fn product_rows(products: List(Product)) : List(List(Text))
  products.map(fn(product)
    [product.name, product.category, product.price.to_fixed(2), product.stock.to_text]
  end)
end

fn sample_products() : List(Product)
  [Product(name: "Tea", category: "Drinks", price: 4.50, stock: 12),
    Product(name: "Notebook", category: "Stationery", price: 7.25, stock: 8),
    Product(name: "Coffee", category: "Drinks", price: 9.00, stock: 6),
    Product(name: "Tea", category: "Drinks", price: 4.50, stock: 12)]
end

fn main(console: Console)
  products = deduplicate(sample_products())
  console.print("Inventory")
  console.print(table(["Product", "Category", "Price", "Stock"], product_rows(products)))
  console.print("\nTop two by price")
  console.print(table(["Product", "Category", "Price", "Stock"],
    product_rows(top_n(products, 2, fn(product) product.price end))))
  console.print("\nGrouped by category")
  for (category, members) in group_items(products, fn(product) product.category end)
    console.print("#{category}: #{members.map(fn(member) member.name end).join(", ")}")
  end
end

test "generic helpers keep stable order and handle an empty selection"
  assert top_n([2, 5, 1, 5], 2, fn(n) n end) == [5, 5]
  assert top_n([2, 5], -1, fn(n) n end) == []
  assert deduplicate([3, 1, 3, 2, 1]) == [3, 1, 2]
  assert group_items([1, 2, 3], fn(n) n % 2 end) == {1: [1, 3], 0: [2]}
  assert table(["A", "B"], [["xx", "z"], ["q"]]) == "A   B\n--  -\nxx  z\nq"
end
