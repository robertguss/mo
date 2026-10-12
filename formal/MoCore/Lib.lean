/-
  Mo Core: library functions written in Core.

  The library functions that take functions (`map`, `filter`, `sort_by`,
  `update`, ...) are Core code, not primitives. A generic library function is
  a Lean function from types to a Core `FunDecl`: instantiating it is
  monomorphization, and each program includes the instances it uses, named
  like `List.map[Text,Int]`. Each loop is a pair of join points (see
  `DSL.forList`). TRANSLATION.md lists which library functions are primitives
  and which are written here.
-/
import MoCore.DSL

namespace MoCore.Lib

open MoCore DSL

/-- The name of a library function instantiated at types. -/
def inst (base : String) (ts : List Ty) : String :=
  base ++ "[" ++ ",".intercalate (ts.map Ty.render) ++ "]"

/-- The types whose order is built in. -/
def baseOf? : Ty → Option Base
  | .int => some .int | .bigInt => some .bigInt | .byte => some .byte | .decimal => some .decimal
  | .float => some .float | .bool => some .bool | .char => some .char
  | .text => some .text | .bytes => some .bytes | .time => some .time
  | .duration => some .duration
  | _ => none

/-- Whether `a` comes before `b` in the order of type `t` (reference 9.2), as
`sort`, `sort_by`, `min` and `max` use it. For a type with a built-in order it
is the `<` primitive, except that a `Float` that is "not a number" sorts last,
where IEEE's `<` would leave it unordered; `Unit`'s one value never comes
before itself. Otherwise it is the type's `compare` function, `compare[T]`,
which the lowering derives (tuples part by part from the left, lists element
by element, records field by field and choice types case by case, in
declaration order) or takes from the type's module. -/
def lt (t : Ty) (a b : Expr) : Expr :=
  match t with
  | .unit => lets [("%lt.a", a), ("%lt.b", b)] ff
  | .float =>
    lets [("%lt.a", a), ("%lt.b", b)] <|
      ite (prim (.float .isNan) [v "%lt.a"]) ff <|
        ite (prim (.float .isNan) [v "%lt.b"]) tt (cmp .float .lt (v "%lt.a") (v "%lt.b"))
  | _ =>
    match baseOf? t with
    | some bt => cmp bt .lt a b
    | none => eq .ordering (call (inst "compare" [t]) [a, b]) (.con .ordering "Less" [])

/-! ## Lists -/

/-- `xs.map(f)`. -/
def map (a b : Ty) : FunDecl :=
  fn (inst "List.map" [a, b]) [("xs", tList a), ("f", tFn [a] b)] (tList b) <|
    forList "l" a "x" (v "xs") [("acc", tList b)] [nil b]
      (nextRound "l" [lop b .append [v "acc", app (v "f") [v "x"]]])
      (v "acc")

/-- `xs.filter(pred)`. -/
def filter (a : Ty) : FunDecl :=
  fn (inst "List.filter" [a]) [("xs", tList a), ("pred", tFn [a] tBool)] (tList a) <|
    forList "l" a "x" (v "xs") [("acc", tList a)] [nil a]
      (ite (app (v "pred") [v "x"])
        (nextRound "l" [lop a .append [v "acc", v "x"]])
        (nextRound "l" [v "acc"]))
      (v "acc")

/-- `xs.filter_map(f)`: the values in the `Some`s. -/
def filterMap (a b : Ty) : FunDecl :=
  fn (inst "List.filter_map" [a, b]) [("xs", tList a), ("f", tFn [a] (tOpt b))] (tList b) <|
    forList "l" a "x" (v "xs") [("acc", tList b)] [nil b]
      (caseOpt (app (v "f") [v "x"]) "y"
        (nextRound "l" [lop b .append [v "acc", v "y"]])
        (nextRound "l" [v "acc"]))
      (v "acc")

/-- `xs.find(pred)`: the first element that satisfies `pred`. -/
def find (a : Ty) : FunDecl :=
  fn (inst "List.find" [a]) [("xs", tList a), ("pred", tFn [a] tBool)] (tOpt a) <|
    forList "l" a "x" (v "xs") [] []
      (ite (app (v "pred") [v "x"]) (some_ a (v "x")) (nextRound "l" []))
      (none_ a)

/-- `xs.any?(pred)`. -/
def any (a : Ty) : FunDecl :=
  fn (inst "List.any?" [a]) [("xs", tList a), ("pred", tFn [a] tBool)] tBool <|
    forList "l" a "x" (v "xs") [] []
      (ite (app (v "pred") [v "x"]) tt (nextRound "l" []))
      ff

/-- `xs.all?(pred)`. -/
def all (a : Ty) : FunDecl :=
  fn (inst "List.all?" [a]) [("xs", tList a), ("pred", tFn [a] tBool)] tBool <|
    forList "l" a "x" (v "xs") [] []
      (ite (app (v "pred") [v "x"]) (nextRound "l" []) ff)
      tt

/-- `xs.count(pred)`. -/
def count (a : Ty) : FunDecl :=
  fn (inst "List.count" [a]) [("xs", tList a), ("pred", tFn [a] tBool)] tInt <|
    forList "l" a "x" (v "xs") [("n", tInt)] [int 0]
      (ite (app (v "pred") [v "x"]) (nextRound "l" [iadd (v "n") (int 1)]) (nextRound "l" [v "n"]))
      (v "n")

/-- `xs.reduce(start, f)`, from the left. -/
def reduce (a b : Ty) : FunDecl :=
  fn (inst "List.reduce" [a, b]) [("xs", tList a), ("start", b), ("f", tFn [b, a] b)] b <|
    forList "l" a "x" (v "xs") [("acc", b)] [v "start"]
      (nextRound "l" [app (v "f") [v "acc", v "x"]])
      (v "acc")

/-- `xs.max`: the largest element, or `None` for an empty list. -/
def max (t : Ty) : FunDecl :=
  fn (inst "List.max" [t]) [("xs", tList t)] (tOpt t) <|
    caseOpt (lop t .uncons [v "xs"]) "p"
      (forList "l" t "x" (snd (v "p")) [("best", t)] [fst (v "p")]
        (ite (lt t (v "best") (v "x")) (nextRound "l" [v "x"]) (nextRound "l" [v "best"]))
        (some_ t (v "best")))
      (none_ t)

/-- `xs.sum` at a number type, with its addition. -/
def sum (t : Ty) (zero : Expr) (add : Expr → Expr → Expr) : FunDecl :=
  fn (inst "List.sum" [t]) [("xs", tList t)] t <|
    forList "l" t "x" (v "xs") [("acc", t)] [zero]
      (nextRound "l" [add (v "acc") (v "x")])
      (v "acc")

/-- Inserts `x` into a list sorted by key, largest first, after every element
whose key is at least as large: a stable insertion. -/
def insertByDesc (a k : Ty) : FunDecl :=
  fn (inst "List.insert_by_desc" [a, k]) [("sorted", tList a), ("x", a), ("key", tFn [a] k)] (tList a) <|
    lets [("kx", app (v "key") [v "x"])] <|
      forList "l" a "y" (v "sorted") [("before", tList a)] [nil a]
        (ite (lt k (app (v "key") [v "y"]) (v "kx"))
          (lop a .concat [v "before", lop a .prepend [lop a .prepend [v "l.rest", v "y"], v "x"]])
          (nextRound "l" [lop a .append [v "before", v "y"]]))
        (lop a .append [v "before", v "x"])

/-- `xs.sort_by_desc(key)`: largest key first; equal keys keep their order
(reference 13.3). An insertion sort: Core states the result, not the cost. -/
def sortByDesc (a k : Ty) : List FunDecl :=
  [insertByDesc a k,
   fn (inst "List.sort_by_desc" [a, k]) [("xs", tList a), ("key", tFn [a] k)] (tList a) <|
    forList "l" a "x" (v "xs") [("acc", tList a)] [nil a]
      (nextRound "l" [call (inst "List.insert_by_desc" [a, k]) [v "acc", v "x", v "key"]])
      (v "acc")]

/-- Inserts `x` into a list sorted by key, smallest first, after every element
whose key is not larger: a stable insertion. -/
def insertBy (a k : Ty) : FunDecl :=
  fn (inst "List.insert_by" [a, k]) [("sorted", tList a), ("x", a), ("key", tFn [a] k)] (tList a) <|
    lets [("kx", app (v "key") [v "x"])] <|
      forList "l" a "y" (v "sorted") [("before", tList a)] [nil a]
        (ite (lt k (v "kx") (app (v "key") [v "y"]))
          (lop a .concat [v "before", lop a .prepend [lop a .prepend [v "l.rest", v "y"], v "x"]])
          (nextRound "l" [lop a .append [v "before", v "y"]]))
        (lop a .append [v "before", v "x"])

/-- `xs.sort_by(key)`: smallest key first, stable. -/
def sortBy (a k : Ty) : List FunDecl :=
  [insertBy a k,
   fn (inst "List.sort_by" [a, k]) [("xs", tList a), ("key", tFn [a] k)] (tList a) <|
    forList "l" a "x" (v "xs") [("acc", tList a)] [nil a]
      (nextRound "l" [call (inst "List.insert_by" [a, k]) [v "acc", v "x", v "key"]])
      (v "acc")]

/-! ## Maps -/

/-- `m.update(key, default, f)`: sets the key to `f` of its value, or of
`default` when it has none. -/
def update (k val : Ty) : FunDecl :=
  fn (inst "Map.update" [k, val]) [("m", tMap k val), ("key", k), ("default", val), ("f", tFn [val] val)]
    (tMap k val) <|
    mop k val .set [v "m", v "key",
      app (v "f") [orElse (mop k val .get [v "m", v "key"]) (v "default")]]

/-! ## Text forms -/

/-- `FileError`'s text form, which reads like `notes/a.md: no such file`. -/
def fileErrorText : FunDecl :=
  fn "FileError.to_text" [("e", tFileError)] tText <|
    .case_ (v "e")
      [("NotFound", ["p"], cat [v "p", str ": no such file"]),
       ("NotAllowed", ["p"], cat [v "p", str ": not allowed"]),
       ("NotText", ["p"], cat [v "p", str ": not text"]),
       ("Failed", ["p", "why"], cat [v "p", str ": ", v "why"])]

/-! ## Calling the instances -/

def mapL (a b : Ty) (xs f : Expr) : Expr := call (inst "List.map" [a, b]) [xs, f]
def filterL (a : Ty) (xs pred : Expr) : Expr := call (inst "List.filter" [a]) [xs, pred]
def filterMapL (a b : Ty) (xs f : Expr) : Expr := call (inst "List.filter_map" [a, b]) [xs, f]
def findL (a : Ty) (xs pred : Expr) : Expr := call (inst "List.find" [a]) [xs, pred]
def anyL (a : Ty) (xs pred : Expr) : Expr := call (inst "List.any?" [a]) [xs, pred]
def allL (a : Ty) (xs pred : Expr) : Expr := call (inst "List.all?" [a]) [xs, pred]
def countL (a : Ty) (xs pred : Expr) : Expr := call (inst "List.count" [a]) [xs, pred]
def reduceL (a b : Ty) (xs start f : Expr) : Expr := call (inst "List.reduce" [a, b]) [xs, start, f]
def maxL (t : Ty) (xs : Expr) : Expr := call (inst "List.max" [t]) [xs]
def sumL (t : Ty) (xs : Expr) : Expr := call (inst "List.sum" [t]) [xs]
def sortByDescL (a k : Ty) (xs key : Expr) : Expr := call (inst "List.sort_by_desc" [a, k]) [xs, key]
def sortByL (a k : Ty) (xs key : Expr) : Expr := call (inst "List.sort_by" [a, k]) [xs, key]
def updateL (k val : Ty) (m key default f : Expr) : Expr :=
  call (inst "Map.update" [k, val]) [m, key, default, f]
def fileErrorTextL (e : Expr) : Expr := call "FileError.to_text" [e]

/-! ## A check: "not a number" sorts last (reference 9.2)

`[NaN, 2.0, 1.0].sort_by(fn(x) x end)` is `[1.0, 2.0, NaN]`. -/

#guard
  let nan : Expr := .lit (.float 0x7FF8000000000000)
  let fl (x : Float) : Expr := .lit (.float x.toBits)
  let P : Program := { data := [], funs := sortBy tFloat tFloat }
  holds P <|
    lets [("s", sortByL tFloat tFloat (listOf tFloat [nan, fl 2, fl 1]) (lam [("x", tFloat)] (v "x")))] <|
      and_ (eq (tList tFloat) (lop tFloat .take [v "s", int 2]) (listOf tFloat [fl 1, fl 2]))
        (caseOpt (lop tFloat .last [v "s"]) "x" (prim (.float .isNan) [v "x"]) ff)

/-! ## A check: `nan`, `inf` and `-inf` as text (reference 9.2)

`x.to_text` and `"#{x}"` both lower to `%float.toText(x)` (TRANSLATION.md
3.4). With `zero = 0.to_float` and `one = 1.to_float`:
`(zero / zero).to_text == "nan"`, and so is `(-(zero / zero)).to_text`;
`(one / zero).to_text == "inf"`; and `(-one / zero).to_text == "-inf"`. -/

#guard
  let text (e : Expr) : Expr := prim (.float .toText) [e]
  let div (a b : Expr) : Expr := prim (.float .div) [a, b]
  let P : Program := { data := [], funs := [] }
  holds P <|
    lets [("zero", iop .toFloat [int 0]), ("one", iop .toFloat [int 1])] <|
      and_ (teq (text (div (v "zero") (v "zero"))) (str "nan")) <|
      and_ (teq (text (prim (.float .neg) [div (v "zero") (v "zero")])) (str "nan")) <|
      and_ (teq (text (div (v "one") (v "zero"))) (str "inf"))
        (teq (text (div (prim (.float .neg) [v "one"]) (v "zero"))) (str "-inf"))

end MoCore.Lib
