/-
  examples/inventory.mo in Core.

  Exercises: generic functions and a generic record, instantiated per type
  before Core (monomorphization: `top_by[Int,Int]`, `Column(Product)`, ...),
  abilities becoming specific primitives (`Order` on `Int` and `Decimal`,
  `Hash` and `Equal` on map keys and set members), functions as values (a
  record field holding a function), sets, and ranges.
-/
import MoCore.Testing

namespace MoCore.Examples.Inventory

open MoCore DSL Lib

/-- `struct Product  name: Text  category: Text  price: Decimal  stock: Int  end` -/
def Product : DataDecl :=
  record "Product" [("name", tText), ("category", tText), ("price", tDec), ("stock", tInt)]

/-- `struct Column(T)  title: Text  cell: fn(T) : Text  numeric?: Bool  end`, instantiated at `T`. -/
def Column (t : Ty) : DataDecl :=
  record s!"Column({t.render})" [("title", tText), ("cell", tFn [t] tText), ("numeric?", tBool)]

/-- ```
fn top_by(items: List(T), n: Int, key: fn(T) : K) : List(T) where K: Order
  items.sort_by_desc(key).take(n)
end
``` -/
def topBy (t k : Ty) : FunDecl :=
  fn (inst "top_by" [t, k]) [("items", tList t), ("n", tInt), ("key", tFn [t] k)] (tList t) <|
    lop t .take [sortByDescL t k (v "items") (v "key"), v "n"]

/-- ```
fn count_by(items: List(T), key: fn(T) : K) : Map(K, Int) where K: Hash
  var counts = {}
  for item in items
    counts = counts.update(key(item), 0, fn(n) n + 1 end)
  end
  counts
end
``` -/
def countBy (t k : Ty) : FunDecl :=
  fn (inst "count_by" [t, k]) [("items", tList t), ("key", tFn [t] k)] (tMap k tInt) <|
    lets [("counts", emptyMap k tInt)] <|
      forList "l" t "item" (v "items") [("counts", tMap k tInt)] [v "counts"]
        (nextRound "l" [updateL k tInt (v "counts") (app (v "key") [v "item"]) (int 0)
          (lam [("n", tInt)] (iadd (v "n") (int 1)))])
        (v "counts")

/-- ```
fn first_by(items: List(T), key: fn(T) : K) : List(T) where K: Hash
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
``` -/
def firstBy (t k : Ty) : FunDecl :=
  fn (inst "first_by" [t, k]) [("items", tList t), ("key", tFn [t] k)] (tList t) <|
    lets [("seen", emptySet k), ("kept", nil t)] <|
      forList "l" t "item" (v "items") [("seen", tSet k), ("kept", tList t)] [v "seen", v "kept"]
        (lets [("k", app (v "key") [v "item"])] <|
          ite (sop k .has [v "seen", v "k"]) (nextRound "l" [v "seen", v "kept"]) <|
            lets [("seen", sop k .add [v "seen", v "k"]),
                  ("kept", lop t .append [v "kept", v "item"])] <|
              nextRound "l" [v "seen", v "kept"])
        (v "kept")

/-- ```
fn table(rows: List(T), columns: List(Column(T))) : Text
  laid_out = columns.map(fn(column) pad_column(rows, column) end)
  lines = (0..rows.size + 1).to_list.map(fn(i)
    laid_out.map(fn(cells) cells.get(i) or "" end).join("  ").trim_end
  end)
  lines.join("\n")
end
``` -/
def table (t : Ty) : FunDecl :=
  let col := (Column t).ty
  fn (inst "table" [t]) [("rows", tList t), ("columns", tList col)] tText <|
    lets [("laid_out", mapL col (tList tText) (v "columns")
            (lam [("column", col)] (call (inst "pad_column" [t]) [v "rows", v "column"]))),
          ("lines", mapL tInt tText (prim (.range .toList) [range (int 0) (iadd (lop t .size [v "rows"]) (int 1))])
            (lam [("i", tInt)]
              (top .trimEnd [top .join [mapL (tList tText) tText (v "laid_out")
                (lam [("cells", tList tText)] (orElse (lop tText .get [v "cells", v "i"]) (str ""))), str "  "]])))] <|
      top .join [v "lines", str "\n"]

/-- ```
fn pad_column(rows: List(T), column: Column(T)) : List(Text)
  cells = [column.title].concat(rows.map(column.cell))
  width = cells.map(fn(cell) cell.char_count end).max or 0
  cells.map(fn(cell) if column.numeric?: cell.pad_left(width) else: cell.pad_right(width) end)
end
```
`rows.map(column.cell)` passes the function held in the field. -/
def padColumn (t : Ty) : FunDecl :=
  let C := Column t
  fn (inst "pad_column" [t]) [("rows", tList t), ("column", C.ty)] (tList tText) <|
    lets [("cells", lop tText .concat [listOf tText [get C "title" (v "column")],
            mapL t tText (v "rows") (get C "cell" (v "column"))]),
          ("width", orElse (maxL tInt (mapL tText tInt (v "cells")
            (lam [("cell", tText)] (top .charCount [v "cell"])))) (int 0))] <|
      mapL tText tText (v "cells")
        (lam [("cell", tText)]
          (ite (get C "numeric?" (v "column")) (top .padLeft [v "cell", v "width"]) (top .padRight [v "cell", v "width"])))

def productE (name category price : String) (stock : Int) : Expr :=
  mk Product [str name, str category, dec price, int stock]

/-- The five products of the sample, as constants. -/
def kettle : FunDecl := const "kettle" Product.ty (productE "Kettle" "kitchen" "39.90" 12)
def lamp : FunDecl := const "lamp" Product.ty (productE "Desk lamp" "office" "24.50" 40)
def knife : FunDecl := const "knife" Product.ty (productE "Chef's knife" "kitchen" "89.00" 5)
def notebook : FunDecl := const "notebook" Product.ty (productE "Notebook" "office" "3.20" 300)
def doormat : FunDecl := const "doormat" Product.ty (productE "Doormat" "home" "19.99" 0)

/-- `products = [kettle, lamp, knife, notebook, doormat]` -/
def products : FunDecl :=
  const "products" (tList Product.ty) <|
    listOf Product.ty [call "kettle" [], call "lamp" [], call "knife" [], call "notebook" [], call "doormat" []]

/-- ```
fn product_columns() : List(Column(Product))
  [
    Column(title: "product", cell: fn(p) p.name end, numeric?: false),
    Column(title: "category", cell: fn(p) p.category end, numeric?: false),
    Column(title: "price", cell: fn(p) p.price.to_fixed(2) end, numeric?: true),
    Column(title: "stock", cell: fn(p) "#{p.stock}" end, numeric?: true)
  ]
end
``` -/
def productColumns : FunDecl :=
  let C := Column Product.ty
  fn "product_columns" [] (tList C.ty) <|
    listOf C.ty
      [mk C [str "product", lam [("p", Product.ty)] (get Product "name" (v "p")), ff],
       mk C [str "category", lam [("p", Product.ty)] (get Product "category" (v "p")), ff],
       mk C [str "price", lam [("p", Product.ty)] (dop .toFixed [get Product "price" (v "p"), int 2]), tt],
       mk C [str "stock", lam [("p", Product.ty)] (intText (get Product "stock" (v "p"))), tt]]

/-- `fn stock_value(product: Product) : Decimal  product.price * product.stock.to_decimal  end` -/
def stockValue : FunDecl :=
  fn "stock_value" [("product", Product.ty)] tDec <|
    dmul (get Product "price" (v "product")) (iop .toDecimal [get Product "stock" (v "product")])

/-- ```
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
``` -/
def mainFn : FunDecl :=
  fn "main" [("console", tCap .console)] tUnit <|
    seq (print (v "console") (str "The three most valuable stocks:")) <|
    seq (print (v "console")
      (call (inst "table" [Product.ty])
        [call (inst "top_by" [Product.ty, tDec])
          [call "products" [], int 3, lam [("p", Product.ty)] (call "stock_value" [v "p"])],
         call "product_columns" []])) <|
    seq (print (v "console") (str "")) <|
      forList "c" (tTup [tText, tInt]) "%e"
        (mop tText tInt .entries [call (inst "count_by" [Product.ty, tText])
          [call "products" [], lam [("p", Product.ty)] (get Product "category" (v "p"))]]) [] []
        (lets [("category", fst (v "%e")), ("count", snd (v "%e"))] <|
          seq (print (v "console") (cat [v "category", str ": ", intText (v "count")])) (nextRound "c" []))
        (seq (print (v "console") (str "")) <|
          lets [("picks", mapL Product.ty tText
                  (call (inst "first_by" [Product.ty, tText])
                    [call "products" [], lam [("p", Product.ty)] (get Product "category" (v "p"))])
                  (lam [("p", Product.ty)] (get Product "name" (v "p"))))] <|
            print (v "console") (cat [str "One from each category: ", top .join [v "picks", str ", "]]))

def program : Program :=
  { data := [Product, Column tInt, Column Product.ty]
    funs := [topBy tInt tInt, topBy tText tInt, topBy Product.ty tDec,
             countBy tText tInt, countBy tInt tInt, countBy Product.ty tText,
             firstBy tText (tOpt tChar), firstBy tInt tInt, firstBy Product.ty tText,
             table tInt, table Product.ty, padColumn tInt, padColumn Product.ty,
             kettle, lamp, knife, notebook, doormat, products, productColumns, stockValue, mainFn,
             Lib.map (Column tInt).ty (tList tText), Lib.map (Column Product.ty).ty (tList tText),
             Lib.map tInt tText, Lib.map (tList tText) tText, Lib.map tText tInt, Lib.map tText tText,
             Lib.map Product.ty tText, Lib.max tInt, Lib.update tInt tInt, Lib.update tText tInt] ++
            Lib.sortByDesc tInt tInt ++ Lib.sortByDesc tText tInt ++ Lib.sortByDesc Product.ty tDec }

/-- The checker accepts the program; `decide +kernel` has the kernel run it. -/
theorem checked : wellTyped? program = true := by decide +kernel

/-- So the program is well typed, and `type_safety` applies to it. -/
theorem wellTyped : WellTyped program := checkProgram_sound checked

/-! ### Examples -/

def ints (ns : List Int) : Expr := listOf tInt (ns.map int)
def idInt : Expr := lam [("x", tInt)] (v "x")

-- top_by([3, 1, 4, 1, 5], 2, fn(x) x end) == [5, 4]
#guard passes program (eq (tList tInt) (call (inst "top_by" [tInt, tInt]) [ints [3, 1, 4, 1, 5], int 2, idInt]) (ints [5, 4]))
-- top_by(["bb", "a", "ccc"], 1, fn(s) s.char_count end) == ["ccc"]
#guard passes program
  (eq (tList tText)
    (call (inst "top_by" [tText, tInt]) [strs ["bb", "a", "ccc"], int 1, lam [("s", tText)] (top .charCount [v "s"])])
    (strs ["ccc"]))
-- top_by([1, 2], 5, fn(x) x end) == [2, 1]
#guard passes program (eq (tList tInt) (call (inst "top_by" [tInt, tInt]) [ints [1, 2], int 5, idInt]) (ints [2, 1]))
-- count_by(["apple", "bean", "avocado"], fn(s) s.char_count end) == {5: 1, 4: 1, 7: 1}
#guard passes program
  (eq (tMap tInt tInt)
    (call (inst "count_by" [tText, tInt]) [strs ["apple", "bean", "avocado"], lam [("s", tText)] (top .charCount [v "s"])])
    (mapOf tInt tInt [(int 5, int 1), (int 4, int 1), (int 7, int 1)]))
-- count_by([1, 2, 3, 4], fn(n) n % 2 end) == {1: 2, 0: 2}
#guard passes program
  (eq (tMap tInt tInt)
    (call (inst "count_by" [tInt, tInt]) [ints [1, 2, 3, 4], lam [("n", tInt)] (irem (v "n") (int 2))])
    (mapOf tInt tInt [(int 1, int 2), (int 0, int 2)]))
-- Maps are equal when they hold the same entries, in any order (reference 3.8):
#guard passes program
  (eq (tMap tInt tInt)
    (call (inst "count_by" [tInt, tInt]) [ints [1, 2, 3, 4], lam [("n", tInt)] (irem (v "n") (int 2))])
    (mapOf tInt tInt [(int 0, int 2), (int 1, int 2)]))
-- first_by(["apple", "bean", "avocado"], fn(s) s.chars.first end) == ["apple", "bean"]
#guard passes program
  (eq (tList tText)
    (call (inst "first_by" [tText, tOpt tChar])
      [strs ["apple", "bean", "avocado"], lam [("s", tText)] (lop tChar .first [top .chars [v "s"]])])
    (strs ["apple", "bean"]))
-- first_by([3, 3, 1], fn(n) n end) == [3, 1]
#guard passes program (eq (tList tInt) (call (inst "first_by" [tInt, tInt]) [ints [3, 3, 1], idInt]) (ints [3, 1]))
-- table([1, 22], [Column(title: "n", cell: fn(n) "#{n}" end, numeric?: true)]) == " n\n 1\n22"
#guard passes program
  (teq (call (inst "table" [tInt])
          [ints [1, 22], listOf (Column tInt).ty [mk (Column tInt) [str "n", lam [("n", tInt)] (intText (v "n")), tt]]])
    (str " n\n 1\n22"))

/-! ### Test "the table lines up text on the left and numbers on the right"

```
expected = """
  product   category  price  stock
  Kettle    kitchen   39.90     12
  Notebook  office     3.20    300
  """
assert table([kettle, notebook], product_columns()) == expected
``` -/
#guard passes program
  (teq (call (inst "table" [Product.ty]) [listOf Product.ty [call "kettle" [], call "notebook" []], call "product_columns" []])
    (str "product   category  price  stock\nKettle    kitchen   39.90     12\nNotebook  office     3.20    300"))

/-! ### Test "the most valuable stock comes first"

```
assert top_by(products, 2, stock_value) == [lamp, notebook]
``` -/
#guard passes program
  (eq (tList Product.ty)
    (call (inst "top_by" [Product.ty, tDec]) [call "products" [], int 2, lam [("p", Product.ty)] (call "stock_value" [v "p"])])
    (listOf Product.ty [call "lamp" [], call "notebook" []]))

/-! ### More: what `main` prints (worked out by hand from the sample's code) -/
#guard passes program
  (seq (call "main" [v "console"]) <|
    eq (tList tText) (io .printed [v "console"])
      (strs ["The three most valuable stocks:",
             "product    category  price  stock\nDesk lamp  office    24.50     40\nNotebook   office     3.20    300\nKettle     kitchen   39.90     12",
             "", "kitchen: 2", "office: 2", "home: 1", "",
             "One from each category: Kettle, Desk lamp, Doormat"]))
  {} [("console", .console)]

/-! ### More: map literals and `to_map` (reference 3.8, 14)

A map literal whose keys turn out equal as the program runs is a bug; `to_map`
keeps the later value of a repeated key, in the key's first place. -/

def pairTy : Ty := tTup [tInt, tInt]
def pairs (ps : List (Int × Int)) : Expr := listOf pairTy (ps.map fun (a, b) => tup [int a, int b])

-- a = 1, b = 1, {a: 1, b: 2} stops
#guard stops program (lets [("a", int 1), ("b", int 1)] (mapOf tInt tInt [(v "a", int 1), (v "b", int 2)]))
  (tMap tInt tInt) .repeatedKey
-- [(1, 1), (2, 2), (1, 3)].to_map.entries == [(1, 3), (2, 2)]
#guard passes program
  (eq (tList pairTy) (mop tInt tInt .entries [mop tInt tInt .ofList [pairs [(1, 1), (2, 2), (1, 3)]]])
    (pairs [(1, 3), (2, 2)]))

/-! ### Never wrong -/

/-- Running `main` never goes wrong. -/
theorem main_never_wrong (n : Nat) (w : World) (msg : String) :
    eval n program (handles [("console", .console)]) w (call "main" [v "console"]) ≠ .wrong msg :=
  never_wrong_with_handles (τ := tUnit) checked (by decide +kernel) n w msg

end MoCore.Examples.Inventory
