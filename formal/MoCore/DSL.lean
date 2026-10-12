/-
  Mo Core: a small DSL for writing Core programs by hand.

  Plain Lean functions that build `Expr`, `FunDecl` and `DataDecl` values, named
  after the Mo constructs they lower (see TRANSLATION.md). Names that start
  with `%` are made by the lowering; Mo names never start with `%`, so they
  can't clash with the program's own.
-/
import MoCore.Eval

namespace MoCore.DSL

open MoCore

/-! ## Types -/

abbrev tInt : Ty := .int
abbrev tBig : Ty := .bigInt
abbrev tByte : Ty := .byte
abbrev tDec : Ty := .decimal
abbrev tFloat : Ty := .float
abbrev tBool : Ty := .bool
abbrev tChar : Ty := .char
abbrev tText : Ty := .text
abbrev tBytes : Ty := .bytes
abbrev tUnit : Ty := .unit
abbrev tRange : Ty := .range
abbrev tTime : Ty := .time
abbrev tDur : Ty := .duration
abbrev tFileError : Ty := .fileError
def tList (t : Ty) : Ty := .list t
def tMap (k v : Ty) : Ty := .map k v
def tSet (t : Ty) : Ty := .set t
def tOpt (t : Ty) : Ty := .option t
def tRes (t e : Ty) : Ty := .result t e
def tTup (ts : List Ty) : Ty := .tuple ts
def tFn (ps : List Ty) (r : Ty) : Ty := .fn ps r
def tCap (c : Cap) : Ty := .cap c

/-! ## Literals -/

def int (n : Int) : Expr := .lit (.int n)
/-- A byte literal, from 0 to 255. -/
def byteE (n : Int) : Expr := .lit (.byte n)
/-- A decimal literal, written as in Mo: `dec "12.50"`. -/
def dec (s : String) : Expr := .lit (.decimal (decLit s))
def str (s : String) : Expr := .lit (.text s)
def chr (c : Char) : Expr := .lit (.char c)
/-- A `Bytes` literal, each byte from 0 to 255. Mo has no way to write one. -/
def bytesE (bs : List Int) : Expr := .lit (.bytes bs)
def bool (b : Bool) : Expr := .lit (.bool b)
def tt : Expr := bool true
def ff : Expr := bool false
def unitE : Expr := .lit .unit
/-- A time literal from RFC 3339 text (for constants such as `t0`). -/
def timeE (s : String) : Expr := .lit (.time (timeLit s))
/-- A duration literal in microseconds. -/
def durE (us : Int) : Expr := .lit (.duration us)

/-! ## Variables, bindings and functions -/

def v (x : String) : Expr := .var x
def global (name : String) : Expr := .global name

/-- `x1 = e1; x2 = e2; ...; body`. -/
def lets : List (String × Expr) → Expr → Expr
  | [], body => body
  | (x, e) :: bs, body => .let_ x e (lets bs body)

/-- A statement whose value is dropped, then the rest. -/
def seq (e rest : Expr) : Expr := .let_ "%_" e rest

def call (f : String) (args : List Expr) : Expr := .call f args
def lam (ps : List (String × Ty)) (body : Expr) : Expr := .lam ps body
def app (f : Expr) (args : List Expr) : Expr := .app f args
def fn (name : String) (ps : List (String × Ty)) (ret : Ty) (body : Expr) : FunDecl :=
  { name, params := ps, ret, body }
/-- A constant declaration, computed once before the program starts. -/
def const (name : String) (ty : Ty) (body : Expr) : ConstDecl := ⟨name, ty, body⟩
/-- Samples conservatively let every function see the full constant prefix;
the front end can give each function its smaller dependency prefix. -/
def wholeProgram (module entry : String) (data : List DataDecl) (consts : List ConstDecl)
    (funs : List FunDecl) : Program :=
  { data, consts, entry, funs := funs.map fun fd => { fd with module, constCount := consts.length } }
def tup (es : List Expr) : Expr := .tuple es
def fst (e : Expr) : Expr := .proj e 0
def snd (e : Expr) : Expr := .proj e 1
def ite (c t f : Expr) : Expr := .if_ c t f
def stop (r : String) : Expr := .stop r
def fail (detail : Expr) : Expr := .fail detail
def prim (p : Prim) (args : List Expr) : Expr := .prim p args

/-- One lowered assertion or example, with a detail supplied by lowering. -/
def assertThat (condition detail : Expr) : Expr := ite condition unitE (fail detail)

/-- A promise's lines, returning the first false line's one-based number. -/
def promiseBody : List Expr → Nat → Expr
  | [], _ => int 0
  | line :: lines, i => ite line (promiseBody lines (i + 1)) (int i)

def promiseFn (name : String) (ps : List (String × Ty)) (lines : List Expr) : FunDecl :=
  { (fn name ps tInt (promiseBody lines 1)) with kind := .promise }

/-- The Boolean form used by the samples' checks of promise answers. -/
def promiseHolds (name : String) (args : List Expr) : Expr :=
  prim (.eq tInt) [call name args, int 0]

/-! ## Booleans and equality -/

def not_ (a : Expr) : Expr := prim .not [a]
/-- `a and b`: the right side only when needed. -/
def and_ (a b : Expr) : Expr := ite a b ff
/-- `a or b`: the right side only when needed. -/
def or_ (a b : Expr) : Expr := ite a tt b
/-- `a implies b`. -/
def implies (a b : Expr) : Expr := ite a b tt
/-- `a == b` at type `t`: structural equality. -/
def eq (t : Ty) (a b : Expr) : Expr := prim (.eq t) [a, b]
def ne (t : Ty) (a b : Expr) : Expr := not_ (eq t a b)

/-! ## Numbers -/

def iop (op : IntOp) (args : List Expr) : Expr := prim (.int op) args
def iadd (a b : Expr) : Expr := iop .add [a, b]
def isub (a b : Expr) : Expr := iop .sub [a, b]
def imul (a b : Expr) : Expr := iop .mul [a, b]
def idiv (a b : Expr) : Expr := iop .div [a, b]
def irem (a b : Expr) : Expr := iop .rem [a, b]
def cmp (b : Base) (op : CmpOp) (x y : Expr) : Expr := prim (.cmp b op) [x, y]
/-- `a..b`: the whole numbers from `a` up to, not including, `b`. -/
def range (a b : Expr) : Expr := prim (.range .make) [a, b]
def ilt (a b : Expr) : Expr := cmp .int .lt a b
def ile (a b : Expr) : Expr := cmp .int .le a b
def igt (a b : Expr) : Expr := cmp .int .gt a b
def ige (a b : Expr) : Expr := cmp .int .ge a b
def ieq (a b : Expr) : Expr := eq .int a b
def intText (a : Expr) : Expr := iop .toText [a]

def dop (op : DecOp) (args : List Expr) : Expr := prim (.dec op) args
def dadd (a b : Expr) : Expr := dop .add [a, b]
def dsub (a b : Expr) : Expr := dop .sub [a, b]
def dmul (a b : Expr) : Expr := dop .mul [a, b]
def ddiv (a b : Expr) : Expr := dop .div [a, b]
def dlt (a b : Expr) : Expr := cmp .decimal .lt a b
def dle (a b : Expr) : Expr := cmp .decimal .le a b
def dgt (a b : Expr) : Expr := cmp .decimal .gt a b
def dge (a b : Expr) : Expr := cmp .decimal .ge a b
def decText (a : Expr) : Expr := dop .toText [a]

/-! ## Text -/

def top (op : TextOp) (args : List Expr) : Expr := prim (.text op) args
/-- Concatenation of the parts of an interpolated text. -/
def cat : List Expr → Expr
  | [] => str ""
  | [e] => e
  | e :: es => top .concat [e, cat es]
def teq (a b : Expr) : Expr := eq .text a b

/-! ## Lists, maps and sets -/

def lop (t : Ty) (op : ListOp) (args : List Expr) : Expr := prim (.list t op) args
/-- A list literal `[e1, ..., en]`. -/
def listOf (t : Ty) (es : List Expr) : Expr := lop t (.lit es.length) es
def nil (t : Ty) : Expr := listOf t []
def mop (k val : Ty) (op : MapOp) (args : List Expr) : Expr := prim (.map k val op) args
def emptyMap (k val : Ty) : Expr := mop k val .empty []
/-- The arguments of a map literal: key, value, key, value, ... -/
def mapLitArgs : List (Expr × Expr) → List Expr
  | [] => []
  | (key, x) :: kvs => key :: x :: mapLitArgs kvs
/-- A map literal `{k1: v1, ...}`: `%map.lit n`, which computes the keys and
values in source order and stops if two keys turn out equal (reference 3.8). -/
def mapOf (k val : Ty) (kvs : List (Expr × Expr)) : Expr :=
  mop k val (.lit kvs.length) (mapLitArgs kvs)
def sop (t : Ty) (op : SetOp) (args : List Expr) : Expr := prim (.set t op) args
/-- `Set.empty`. -/
def emptySet (t : Ty) : Expr := sop t .empty []
/-- A list of texts: `["a", "b"]`. -/
def strs (xs : List String) : Expr := listOf .text (xs.map str)

/-! ## Capabilities -/

def io (op : IoOp) (args : List Expr) : Expr := prim (.io op) args
def print (console text : Expr) : Expr := io .print [console, text]
def printError (console text : Expr) : Expr := io .printError [console, text]
def argsList (args : Expr) : Expr := io .argsList [args]

/-! ## Option and Result -/

def some_ (t : Ty) (e : Expr) : Expr := .con (.option t) "Some" [e]
def none_ (t : Ty) : Expr := .con (.option t) "None" []
def ok_ (t err : Ty) (e : Expr) : Expr := .con (.result t err) "Ok" [e]
def err_ (t err : Ty) (e : Expr) : Expr := .con (.result t err) "Error" [e]
/-- `case e  Some(x): someB  None: noneB  end`. Core's arms follow the
declaration order, `None` then `Some` (reference 8). -/
def caseOpt (e : Expr) (x : String) (someB noneB : Expr) : Expr :=
  .case_ e [("None", [], noneB), ("Some", [x], someB)]
/-- `case e  Ok(x): okB  Error(y): errB  end`. -/
def caseRes (e : Expr) (x : String) (okB : Expr) (y : String) (errB : Expr) : Expr :=
  .case_ e [("Ok", [x], okB), ("Error", [y], errB)]
/-- `e or d`: like `and` and `or`, it skips its right side when the left
decides the answer (reference 13.1), so `d` is evaluated only for `None`. -/
def orElse (e d : Expr) : Expr := caseOpt e "%or" (v "%or") d
/-- `e.or_fail(failure)`, for an `Option(t)` and a failure of type `err`: a
library call, so the option and then the failure are evaluated first. -/
def orFail (t err : Ty) (e failure : Expr) : Expr :=
  lets [("%opt", e), ("%fail", failure)] <|
    caseOpt (v "%opt") "%x" (ok_ t err (v "%x")) (err_ t err (v "%fail"))
/-- `x is Some(_)`. -/
def isSome (e : Expr) : Expr := caseOpt e "%_" tt ff

/-! ## Records and choice types -/

/-- `struct Name ... end`: one constructor named like the type. -/
def record (name : String) (fields : List (String × Ty)) : DataDecl := ⟨name, [⟨name, fields⟩]⟩
/-- `enum Name ... end`: one constructor per case. -/
def choice (name : String) (cases : List (String × List (String × Ty))) : DataDecl :=
  ⟨name, cases.map fun (c, fs) => ⟨c, fs⟩⟩
/-- The type a declaration declares. -/
def _root_.MoCore.DataDecl.ty (d : DataDecl) : Ty := .data d.name
/-- Builds a record from its fields in declaration order. -/
def mk (d : DataDecl) (args : List Expr) : Expr := .con (.data d.name) d.name args
/-- Builds case `c` of a choice type. -/
def mkC (d : DataDecl) (c : String) (args : List Expr) : Expr := .con (.data d.name) c args
/-- The fields of a record's one constructor. -/
def recordFields (d : DataDecl) : List (String × Ty) :=
  match d.ctors with
  | [c] => c.fields
  | _ => []
/-- `e.f`: a one-arm `case` that binds every field. -/
def get (d : DataDecl) (f : String) (e : Expr) : Expr :=
  .case_ e [(d.name, (recordFields d).map (fun p => "%" ++ p.1), v ("%" ++ f))]
/-- The record `e` with field `f` replaced by `new` (assigning a `var`'s field). -/
def setField (d : DataDecl) (f : String) (e new : Expr) : Expr :=
  .let_ "%%new" new <|
    .case_ e [(d.name, (recordFields d).map (fun p => "%" ++ p.1),
      mk d ((recordFields d).map fun p => if p.1 = f then v "%%new" else v ("%" ++ p.1)))]

/-! ## Loops and early exits, with join points

A loop named `lbl` threads its state variables (the `var`s it assigns) as the
parameters of its join points: `lbl.loop` starts a round, `lbl.done` runs what
follows the loop. Every path through a body ends with `nextRound`
(`continue`, or the end of the body) or `breakLoop` (`break`). -/

/-- `for x in xs ... end; after`, over a `List(t)`. -/
def forList (lbl : String) (t : Ty) (x : String) (xs : Expr) (st : List (String × Ty))
    (init : List Expr) (body : Expr) (after : Expr) : Expr :=
  let rest := lbl ++ ".rest"
  let p := lbl ++ ".p"
  .join (lbl ++ ".done") st after <|
    .join (lbl ++ ".loop") ((rest, .list t) :: st)
      (.case_ (lop t .uncons [v rest])
        [("None", [], .jump (lbl ++ ".done") (st.map fun s => v s.1)),
         ("Some", [p], lets [(x, fst (v p)), (rest, snd (v p))] body)])
      (.jump (lbl ++ ".loop") (xs :: init))

/-- The next round of a `for` loop, with the new state. -/
def nextRound (lbl : String) (st : List Expr) : Expr :=
  .jump (lbl ++ ".loop") (v (lbl ++ ".rest") :: st)

/-- `for x in s ... end; after`, over a `Set(t)`: a loop over its elements in
insertion order, `%set.toList(s)`, as a loop over a map takes apart its
entries. `nextRound` and `breakLoop` work as in `forList`. -/
def forSet (lbl : String) (t : Ty) (x : String) (s : Expr) (st : List (String × Ty))
    (init : List Expr) (body : Expr) (after : Expr) : Expr :=
  forList lbl t x (sop t .toList [s]) st init body after

/-- `while ...`: `body` decides, with `nextWhile` or `breakLoop`. -/
def whileLoop (lbl : String) (st : List (String × Ty)) (init : List Expr) (body after : Expr) : Expr :=
  .join (lbl ++ ".done") st after <| .join (lbl ++ ".loop") st body (.jump (lbl ++ ".loop") init)

/-- The next round of a `while` loop, with the new state. -/
def nextWhile (lbl : String) (st : List Expr) : Expr := .jump (lbl ++ ".loop") st

/-- `break`: leave the loop with the state. -/
def breakLoop (lbl : String) (st : List Expr) : Expr := .jump (lbl ++ ".done") st

/-- A function body with early `return`s: `ret e` returns `e`. -/
def withReturn (r : Ty) (body : Expr) : Expr := .join "%return" [("%r", r)] (v "%r") body

/-- `return e`. -/
def ret (e : Expr) : Expr := .jump "%return" [e]

/-- `assert cond`, then the rest of a test. -/
def assert (cond : Expr) (what : String) (rest : Expr) : Expr :=
  ite cond rest (fail (str ("assertion failed: " ++ what)))

end MoCore.DSL

namespace MoCore.DSL

/-- The real capability handles the runtime hands over: each
variable bound to a handle on the folder the program started in. -/
def handles (caps : List (String × Cap)) : Env := caps.map fun (x, c) => (x, .cap c (some ""))

end MoCore.DSL

namespace MoCore.DSL

/-! ## `try`, list patterns and range loops -/

/-- `x = try e` for `e : Result(_, err)`, in a function returning
`Result(r, err)`, where the rest of the body is in tail position: the rest runs
with `x` bound, or the function returns the `Error` (reference 8.3). Inside a
loop or a nested continuation, the lowering jumps to `%return` instead. -/
def tryRes (r err : Ty) (e : Expr) (x : String) (rest : Expr) : Expr :=
  caseRes e x rest "%err" (err_ r err (v "%err"))

/-- `x = try e` with automatic wrapping: the failure type differs, and the
function's failure type has exactly one case that holds it (`wrap`). -/
def tryWrap (r err : Ty) (e : Expr) (x : String) (wrap : Expr → Expr) (rest : Expr) : Expr :=
  caseRes e x rest "%err" (err_ r err (wrap (v "%err")))

/-- `x = try e` for `e : Option(_)`, in a function returning `Option(r)`. -/
def tryOpt (r : Ty) (e : Expr) (x : String) (rest : Expr) : Expr :=
  caseOpt e x rest (none_ r)

/-- A list pattern at position `k` of a decision tree of `uncons`. -/
def matchListAt (t : Ty) (k : Nat) (xs : Expr) : List String → Option String → Expr → Expr → Expr
  | [], none, onMatch, onFail => caseOpt (lop t .uncons [xs]) "%_" onFail onMatch
  | [], some r, onMatch, _ => .let_ r xs onMatch
  | x :: names, rest, onMatch, onFail =>
    caseOpt (lop t .uncons [xs]) s!"%m{k}"
      (lets [(x, fst (v s!"%m{k}"))]
        (matchListAt t (k + 1) (snd (v s!"%m{k}")) names rest onMatch onFail))
      onFail

/-- A list pattern, lowered to a decision tree of `uncons`: `[x1, ..., xn]`
(exactly `n` elements, `rest = none`) or `[x1, ..., xn, ..rest]`. `onFail`
appears at every point where the match can fail, some of them under the
pattern's bindings, so the lowering makes it a jump to a join point (the rest
of the arms), which has no free variables. -/
def matchList (t : Ty) (xs : Expr) (names : List String) (rest : Option String)
    (onMatch onFail : Expr) : Expr :=
  matchListAt t 0 xs names rest onMatch onFail

/-- `for i in r ... end; after`, over a range `r`: counts from its start while
below its end, so `i + 1` (`nextRange`) never passes the end, and the counter
never overflows (architecture 4.3). -/
def forRange (lbl i : String) (r : Expr) (st : List (String × Ty)) (init : List Expr)
    (body after : Expr) : Expr :=
  let rv := lbl ++ ".range"
  lets [(rv, r), (lbl ++ ".end", prim (.range .end) [v rv])] <|
    .join (lbl ++ ".done") st after <|
      .join (lbl ++ ".loop") ((i, .int) :: st)
        (ite (ilt (v i) (v (lbl ++ ".end"))) body (.jump (lbl ++ ".done") (st.map fun s => v s.1)))
        (.jump (lbl ++ ".loop") (prim (.range .start) [v rv] :: init))

/-- The next round of a range loop, with the new state. -/
def nextRange (lbl i : String) (st : List Expr) : Expr :=
  .jump (lbl ++ ".loop") (iadd (v i) (int 1) :: st)

end MoCore.DSL
