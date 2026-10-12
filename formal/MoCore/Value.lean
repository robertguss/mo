/-
  Mo Core: values, the world, and outcomes.
-/
import MoCore.Syntax

namespace MoCore

/-! ## Ranges

Core keeps numbers as Lean's unbounded `Int` and states their ranges as
predicates. Every primitive that makes a number checks its range and stops on
overflow, and the value typing relation (`MoCore/Typing.lean`) includes the
ranges, so type safety implies that no out-of-range number is ever computed. -/

def int64Min : Int := -9223372036854775808
def int64Max : Int := 9223372036854775807

/-- A 64-bit signed whole number. `Int` and `Duration` values are in range. -/
def InInt64 (n : Int) : Prop := int64Min ≤ n ∧ n ≤ int64Max

instance (n : Int) : Decidable (InInt64 n) := inferInstanceAs (Decidable (_ ∧ _))

/-- A `Byte`: a whole number from 0 to 255 (reference 3.2). -/
def InByte (n : Int) : Prop := 0 ≤ n ∧ n ≤ 255

instance (n : Int) : Decidable (InByte n) := inferInstanceAs (Decidable (_ ∧ _))

/-- `10^18`: a `Decimal` is an integer count of `10^-18`. -/
def decScale : Int := 1000000000000000000

/-- The largest `Decimal` count, `2^127 - 1`. -/
def decMax : Int := 170141183460469231731687303715884105727

/-- A `Decimal` count in range: within `±(2^127 - 1)`. -/
def InDecimal (n : Int) : Prop := -decMax ≤ n ∧ n ≤ decMax

instance (n : Int) : Decidable (InDecimal n) := inferInstanceAs (Decidable (_ ∧ _))

/-- 0001-01-01T00:00:00Z, in microseconds since 1970-01-01T00:00:00Z. -/
def timeMin : Int := -62135596800000000

/-- 9999-12-31T23:59:59.999999Z, in microseconds since 1970-01-01T00:00:00Z. -/
def timeMax : Int := 253402300799999999

/-- A `Time` between the years 1 and 9999 (reference 3.9). -/
def InTime (t : Int) : Prop := timeMin ≤ t ∧ t ≤ timeMax

instance (t : Int) : Decidable (InTime t) := inferInstanceAs (Decidable (_ ∧ _))

/-! ## Values -/

/-- Core values.

* Numbers are exact: `byte n` is a whole number from 0 to 255; `decimal n` is
  `n * 10^-18`; `time` and `duration` count microseconds; `float` holds IEEE
  754 bits.
* `bytes bs` is a sequence of bytes, each from 0 to 255.
* `range a b` is the range `a..b`: the whole numbers from `a` up to, not
  including, `b`, none when `b ≤ a`.
* `con c fields` is a constructed value of a record or choice type: its
  constructor's name and its fields, in declaration order.
* `map` is an association list in insertion order, with distinct keys.
* `set` is a list of elements in insertion order, none equal to another
  (value typing requires it: `ValTy.set`).
* A `closure` captures the environment it was made in.
* A capability value `cap c root` is a handle: what it reaches is in the
  `World`. `root` is the folder a `Files` or `ReadFiles` handle is narrowed to
  (`some ""` for the folder the program started in), or `none` for a handle
  that reaches nothing, which `within` a path that would leave the folder
  gives (reference 11.3, 14). Other capabilities ignore it. -/
inductive Value where
  | int (n : Int)
  | bigInt (n : Int)
  | byte (n : Int)
  | decimal (n : Int)
  | float (bits : UInt64)
  | bool (b : Bool)
  | char (c : Char)
  | text (s : String)
  | bytes (bs : List Int)
  | unit
  | range (a b : Int)
  | time (us : Int)
  | duration (us : Int)
  | tuple (vs : List Value)
  | list (vs : List Value)
  | map (es : List (Value × Value))
  | set (vs : List Value)
  | con (c : String) (fields : List Value)
  | closure (env : List (String × Value)) (params : List (String × Ty)) (body : Expr)
      (globals : List (String × Value))
  | cap (c : Cap) (root : Option String) (fake : Option Nat := none)
  deriving Repr, Inhabited

/-- An environment: variables and their values, innermost first. -/
abbrev Env := List (String × Value)

/-- The value of a literal. -/
def Lit.value : Lit → Value
  | .int n => .int n
  | .bigInt n => .bigInt n
  | .byte n => .byte n
  | .decimal n => .decimal n
  | .float b => .float b
  | .bool b => .bool b
  | .char c => .char c
  | .text s => .text s
  | .bytes bs => .bytes bs
  | .unit => .unit
  | .time t => .time t
  | .duration d => .duration d

/-- A literal is well formed when its number is in its type's range, and a
`Bytes` literal when each of its bytes is from 0 to 255. -/
def Lit.Ok : Lit → Prop
  | .int n => InInt64 n
  | .byte n => InByte n
  | .bytes bs => ∀ b ∈ bs, InByte b
  | .decimal n => InDecimal n
  | .time t => InTime t
  | .duration d => InInt64 d
  | _ => True

instance (l : Lit) : Decidable l.Ok := by
  cases l <;> simp only [Lit.Ok] <;> infer_instance

/-- The constructed values of the library's choice types. -/
def someV (v : Value) : Value := .con "Some" [v]
def noneV : Value := .con "None" []
def okV (v : Value) : Value := .con "Ok" [v]
def errorV (v : Value) : Value := .con "Error" [v]

/-! ## The world

Effects go through primitives on capability values, which read and update one
`World` record. A real runtime implements the world with the operating system;
a test's fakes (reference 12.4) are a world built for the test. -/

structure CapState where
  /-- The command-line arguments after the program's name (`Args`). -/
  args : List String := []
  /-- The environment variables (`Env`). -/
  env : List (String × String) := []
  /-- The lines of standard input not yet read (`Console.read_line`). -/
  stdin : List String := []
  /-- The lines printed to standard output, oldest first. -/
  stdout : List String := []
  /-- The lines printed to standard error, oldest first. -/
  stderr : List String := []
  /-- The files: paths (relative, with `/`) and their texts, in insertion order. -/
  files : List (String × String) := []
  /-- The clock's time, in microseconds since 1970-01-01T00:00:00Z. -/
  now : Int := 0
  /-- The random number generator's state. -/
  seed : Nat := 0
  deriving Repr, Inhabited

/-- Real capabilities use the inherited state. Fake handles index the
append-only store; narrowing a handle never copies its state. -/
structure World extends CapState where
  fakes : List CapState := []
  deriving Repr, Inhabited

/-! ## Outcomes -/

/-- The bugs of reference 13.2 that Core can hit. Bugs are never caught. -/
inductive Bug where
  /-- Overflow in `Int`, `Decimal` or `Duration` arithmetic, or a time outside
  the years 1 to 9999. -/
  | overflow (what : String)
  | divByZero
  /-- Reaching `stop "reason"`. -/
  | stop (reason : String)
  /-- Calling a library function outside what it accepts, such as `take(-1)`. -/
  | outside (call : String)
  /-- Two keys of a map literal that turn out equal as the program runs, as
  in `{a: 1, b: 2}` when `a == b` (reference 3.8, 13.2). -/
  | repeatedKey
  deriving Repr, Inhabited, DecidableEq

/-- The outcome of evaluating an expression. -/
inductive Outcome where
  /-- A value, and the world after the effects. -/
  | ok (v : Value) (w : World)
  /-- The program stopped on a bug; the world is as it was at the stop, so what
  was printed before it stays visible. -/
  | stopped (b : Bug) (w : World)
  /-- An assertion or example failed; this is not a bug. -/
  | failed (detail : String) (w : World)
  /-- A jump to a join point, on its way to the `join` that declared it.
  Internal: a well-typed expression never ends this way at top level. -/
  | jumped (j : String) (vs : List Value) (w : World)
  /-- Stuck: only possible for ill-typed programs. -/
  | wrong (msg : String)
  /-- The evaluator ran out of fuel. -/
  | outOfFuel
  deriving Repr, Inhabited

/-- Sequencing: continue with the value, or pass any other outcome on. -/
def Outcome.bind (o : Outcome) (k : Value → World → Outcome) : Outcome :=
  match o with
  | .ok v w => k v w
  | .stopped b w => .stopped b w
  | .failed detail w => .failed detail w
  | .jumped j vs w => .jumped j vs w
  | .wrong m => .wrong m
  | .outOfFuel => .outOfFuel

@[simp] theorem Outcome.bind_ok : (Outcome.ok v w).bind k = k v w := rfl
@[simp] theorem Outcome.bind_stopped : (Outcome.stopped b w).bind k = .stopped b w := rfl
@[simp] theorem Outcome.bind_failed : (Outcome.failed detail w).bind k = .failed detail w := rfl
@[simp] theorem Outcome.bind_jumped : (Outcome.jumped j vs w).bind k = .jumped j vs w := rfl
@[simp] theorem Outcome.bind_wrong : (Outcome.wrong m).bind k = .wrong m := rfl
@[simp] theorem Outcome.bind_outOfFuel : Outcome.outOfFuel.bind k = .outOfFuel := rfl

end MoCore
