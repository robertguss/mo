module Main
use Shop.Catalog

fn main(console: Console)
  console.print(Catalog.describe("pen"))
  console.print("#{Catalog.price("pen") + Catalog.price("ink")}")
end
