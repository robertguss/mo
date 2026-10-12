## Generic inventory helpers and a product report.
module Inventory
expose Product(..), Column(..), top_n, group_items, deduplicate, text_table, product_table
use Unicode16

struct Product
  sku: Text
  name: Text
  category: Text
  price: Decimal
  stock: Int
end

## A column renders one item and chooses its alignment.
struct Column(T)
  heading: Text
  render: fn(T) : Text
  right_align?: Bool
end

## Largest keys first; equal keys retain their input order.
## Nonpositive counts give an empty list; oversized counts keep all items.
fn top_n(items: List(T), n: Int, key: fn(T) : K) : List(T) where K: Order
  items.sort_by_desc(key).take(n.max(0))
end

## Groups and their members follow first appearance in the input.
fn group_items(items: List(T), key: fn(T) : K) : Map(K, List(T)) where K: Hash
  items.group_by(key)
end

## Structural duplicates are removed, keeping the first occurrence.
fn deduplicate(items: List(T)) : List(T) where T: Hash
  items.to_set.to_list
end

## Escape line breaks and tabs so each cell stays on one printed line.
fn cell_text(text: Text) : Text
  text.replace("\r", "\\r").replace("\n", "\\n").replace("\t", "\\t")
end

fn render_row(cells: List(Text), widths: List(Int), alignments: List(Bool)) : Text
  cells.zip(widths).zip(alignments).map(fn(entry)
    cell = entry.0.0
    width = entry.0.1
    padding = " ".repeat((width - Unicode16.display_width(cell)).max(0))
    if entry.1: "#{padding}#{cell}" else: "#{cell}#{padding}"
  end).join(" | ")
end

## Column widths include headings and all rendered values.
## Empty items still produce a header; no columns produce empty text.
fn text_table(items: List(T), columns: List(Column(T))) : Text
  return "" if columns.empty?
  headings = columns.map(fn(column) cell_text(column.heading) end)
  rows = items.map(fn(item)
    columns.map(fn(column) cell_text(column.render(item)) end)
  end)
  widths = headings.enumerate.map(fn(index_heading)
    index = index_heading.0
    heading_width = Unicode16.display_width(index_heading.1)
    value_widths = rows.map(fn(row) Unicode16.display_width(row.get(index) or "") end)
    heading_width.max(value_widths.max or 0)
  end)
  alignments = columns.map(fn(column) column.right_align? end)
  header = render_row(headings, widths, alignments)
  separator = widths.map(fn(width) "-".repeat(width) end).join("-+-")
  body = rows.map(fn(row) render_row(row, widths, alignments) end)
  [header, separator].concat(body).join("\n")
end

fn product_table(products: List(Product)) : Text
  columns = [
    Column(heading: "SKU", render: fn(product) product.sku end, right_align?: false),
    Column(heading: "Name", render: fn(product) product.name end, right_align?: false),
    Column(heading: "Category", render: fn(product) product.category end, right_align?: false),
    Column(heading: "Price", render: fn(product) product.price.to_fixed(2) end, right_align?: true),
    Column(heading: "Stock", render: fn(product) product.stock.to_text end, right_align?: true)
  ]
  text_table(products, columns)
end

fn main(console: Console)
  products = [
    Product(sku: "P100", name: "Pencil", category: "Stationery", price: 0.75, stock: 120),
    Product(sku: "P200", name: "Notebook", category: "Stationery", price: 4.50, stock: 30),
    Product(sku: "P300", name: "Mug", category: "Kitchen", price: 12.95, stock: 18),
    Product(sku: "P400", name: "茶", category: "Kitchen", price: 6.25, stock: 45),
    Product(sku: "P500", name: "Teapot", category: "Kitchen", price: 12.95, stock: 8),
    Product(sku: "P100", name: "Pencil", category: "Stationery", price: 0.75, stock: 120)
  ]
  inventory = deduplicate(products)
  console.print("Inventory: #{inventory.size} products, #{products.size - inventory.size} duplicates removed")
  console.print(product_table(inventory))
  console.print("\nTop three products by price")
  console.print(product_table(top_n(inventory, 3, fn(product) product.price end)))
  for (category, members) in group_items(inventory, fn(product) product.category end)
    console.print("\n#{category}: #{members.size} products")
    console.print(product_table(members))
  end
end
