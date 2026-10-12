/-
  Mo Core: syntax.

  Core is the small, typed, monomorphic, strict functional language that every
  feature of Mo v0.1 lowers to (architecture, section 4). This file defines its
  types, its primitive operations with their typing signatures, its
  expressions, and its programs.

  Core has no type variables and no abilities: generic Mo functions are
  instantiated per type before Core, and each use of an ability becomes the
  specific primitive or function for its type.
-/

namespace MoCore

/-! ## Types -/

/-- The capability types of reference section 11.1. -/
inductive Cap where
  | console | files | readFiles | clock | random | args | env
  deriving DecidableEq, Repr, Inhabited

/-- Core types.

* `data name` is a declared record or choice type, looked up by name in the
  program's data table. Records and choice types share one form: a list of
  constructors, each with fields.
* `option`, `result`, `ordering` and `fileError` are the library's choice types
  that are always in scope (reference 10.5). They are ordinary choice types, but
  their instances are named structurally, so that primitives can return them
  without the program declaring them; `ctorsOf` gives their constructors.
* `map k v` and `set t` are `Map(K, V)` and `Set(T)`, whose values keep
  insertion order (reference 3.8). -/
inductive Ty where
  | int        -- 64-bit, checked
  | bigInt
  | byte       -- a whole number from 0 to 255
  | decimal    -- an integer count of 10^-18, within ±(2^127 - 1)
  | float
  | bool
  | char
  | text
  | bytes      -- a sequence of bytes
  | unit
  | range      -- `a..b`: two `Int`s, its start and its end
  | time       -- microseconds since 1970-01-01T00:00:00Z, years 1 to 9999
  | duration   -- microseconds, 64-bit, checked
  | tuple (ts : List Ty)
  | list (t : Ty)
  | map (k v : Ty)
  | set (t : Ty)
  | data (name : String)
  | option (t : Ty)
  | result (t e : Ty)
  | ordering
  | fileError
  | entry
  | entryKind
  | fn (params : List Ty) (ret : Ty)
  | cap (c : Cap)
  deriving Repr, Inhabited

/-! ### Decidable equality on types

`deriving DecidableEq` does not handle nested inductive types, so we write a
Boolean equality and prove it sound and reflexive. -/

mutual
/-- Boolean equality of types. -/
def Ty.beq : Ty → Ty → Bool
  | .int, .int | .bigInt, .bigInt | .byte, .byte | .decimal, .decimal | .float, .float
  | .bool, .bool | .char, .char | .text, .text | .bytes, .bytes | .unit, .unit
  | .range, .range | .time, .time | .duration, .duration | .ordering, .ordering
  | .fileError, .fileError | .entry, .entry | .entryKind, .entryKind => true
  | .tuple as, .tuple bs => Ty.beqList as bs
  | .list a, .list b => Ty.beq a b
  | .map k v, .map k' v' => Ty.beq k k' && Ty.beq v v'
  | .set a, .set b => Ty.beq a b
  | .data s, .data s' => s == s'
  | .option a, .option b => Ty.beq a b
  | .result a e, .result b e' => Ty.beq a b && Ty.beq e e'
  | .fn ps r, .fn qs s => Ty.beqList ps qs && Ty.beq r s
  | .cap c, .cap d => c == d
  | _, _ => false
/-- Boolean equality of type lists. -/
def Ty.beqList : List Ty → List Ty → Bool
  | [], [] => true
  | a :: as, b :: bs => Ty.beq a b && Ty.beqList as bs
  | _, _ => false
end

mutual
theorem Ty.beq_refl : ∀ (a : Ty), Ty.beq a a = true
  | .int | .bigInt | .byte | .decimal | .float | .bool | .char | .text | .bytes | .unit
  | .range | .time | .duration | .ordering | .fileError | .entry | .entryKind => rfl
  | .tuple as => by simp only [Ty.beq]; exact Ty.beqList_refl as
  | .list a => by simp only [Ty.beq]; exact Ty.beq_refl a
  | .map k v => by simp only [Ty.beq, Ty.beq_refl k, Ty.beq_refl v, Bool.and_self]
  | .set a => by simp only [Ty.beq]; exact Ty.beq_refl a
  | .data s => by simp [Ty.beq]
  | .option a => by simp only [Ty.beq]; exact Ty.beq_refl a
  | .result a e => by simp only [Ty.beq, Ty.beq_refl a, Ty.beq_refl e, Bool.and_self]
  | .fn ps r => by simp only [Ty.beq, Ty.beqList_refl ps, Ty.beq_refl r, Bool.and_self]
  | .cap c => by simp [Ty.beq]
theorem Ty.beqList_refl : ∀ (as : List Ty), Ty.beqList as as = true
  | [] => rfl
  | a :: as => by simp only [Ty.beqList, Ty.beq_refl a, Ty.beqList_refl as, Bool.and_self]
end

mutual
theorem Ty.eq_of_beq : ∀ (a b : Ty), Ty.beq a b = true → a = b
  | .int, b, h | .bigInt, b, h | .byte, b, h | .decimal, b, h | .float, b, h | .bool, b, h
  | .char, b, h | .text, b, h | .bytes, b, h | .unit, b, h | .range, b, h | .time, b, h
  | .duration, b, h | .ordering, b, h | .fileError, b, h
  | .entry, b, h | .entryKind, b, h => by cases b <;> simp_all [Ty.beq]
  | .data s, b, h | .cap s, b, h => by cases b <;> simp_all [Ty.beq]
  | .tuple as, b, h => by
      cases b <;> simp only [Ty.beq, Bool.false_eq_true] at h
      rw [Ty.eq_of_beqList as _ h]
  | .list a, b, h => by
      cases b <;> simp only [Ty.beq, Bool.false_eq_true] at h
      rw [Ty.eq_of_beq a _ h]
  | .map k v, b, h => by
      cases b <;> simp only [Ty.beq, Bool.false_eq_true, Bool.and_eq_true] at h
      rw [Ty.eq_of_beq k _ h.1, Ty.eq_of_beq v _ h.2]
  | .set a, b, h => by
      cases b <;> simp only [Ty.beq, Bool.false_eq_true] at h
      rw [Ty.eq_of_beq a _ h]
  | .option a, b, h => by
      cases b <;> simp only [Ty.beq, Bool.false_eq_true] at h
      rw [Ty.eq_of_beq a _ h]
  | .result a e, b, h => by
      cases b <;> simp only [Ty.beq, Bool.false_eq_true, Bool.and_eq_true] at h
      rw [Ty.eq_of_beq a _ h.1, Ty.eq_of_beq e _ h.2]
  | .fn ps r, b, h => by
      cases b <;> simp only [Ty.beq, Bool.false_eq_true, Bool.and_eq_true] at h
      rw [Ty.eq_of_beqList ps _ h.1, Ty.eq_of_beq r _ h.2]
theorem Ty.eq_of_beqList : ∀ (as bs : List Ty), Ty.beqList as bs = true → as = bs
  | [], bs, h => by cases bs <;> simp_all [Ty.beqList]
  | a :: as, bs, h => by
      cases bs <;> simp only [Ty.beqList, Bool.false_eq_true, Bool.and_eq_true] at h
      rw [Ty.eq_of_beq a _ h.1, Ty.eq_of_beqList as _ h.2]
end

instance : DecidableEq Ty := fun a b =>
  if h : Ty.beq a b = true then isTrue (Ty.eq_of_beq a b h)
  else isFalse (fun e => h (e ▸ Ty.beq_refl a))

/-- The text of a capability type's name. -/
def Cap.name : Cap → String
  | .console => "Console" | .files => "Files" | .readFiles => "ReadFiles"
  | .clock => "Clock" | .random => "Random" | .args => "Args" | .env => "Env"

mutual
/-- A type written as Mo writes it, used to name instantiated functions such
as `List.map[Text,Int]`. -/
def Ty.render : Ty → String
  | .int => "Int" | .bigInt => "BigInt" | .byte => "Byte" | .decimal => "Decimal"
  | .float => "Float" | .bool => "Bool" | .char => "Char" | .text => "Text"
  | .bytes => "Bytes" | .unit => "Unit" | .range => "Range" | .time => "Time"
  | .duration => "Duration" | .ordering => "Ordering" | .fileError => "FileError"
  | .entry => "Entry" | .entryKind => "EntryKind"
  | .tuple ts => "(" ++ Ty.renderList ts ++ ")"
  | .list t => "List(" ++ Ty.render t ++ ")"
  | .map k v => "Map(" ++ Ty.render k ++ ", " ++ Ty.render v ++ ")"
  | .set t => "Set(" ++ Ty.render t ++ ")"
  | .data s => s
  | .option t => "Option(" ++ Ty.render t ++ ")"
  | .result t e => "Result(" ++ Ty.render t ++ ", " ++ Ty.render e ++ ")"
  | .fn ps r => "fn(" ++ Ty.renderList ps ++ ") : " ++ Ty.render r
  | .cap c => c.name
/-- Types separated by commas. -/
def Ty.renderList : List Ty → String
  | [] => ""
  | [t] => Ty.render t
  | t :: ts => Ty.render t ++ ", " ++ Ty.renderList ts
end

instance : ToString Ty := ⟨Ty.render⟩

/-! ## Literals -/

/-- Literal constants. Number literals carry their exact Core representation:
`decimal n` is `n * 10^-18`, `time t` and `duration d` count microseconds, and
`float bits` holds the IEEE 754 bits of a `Float`. Mo has no way to write a
`Bytes` value, but Core has a literal for one, as the bytecode has constants
for them (`bytecode.md` 3.1). -/
inductive Lit where
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
  | time (us : Int)
  | duration (us : Int)
  deriving Repr, Inhabited, DecidableEq

/-- The type of a literal. -/
def Lit.ty : Lit → Ty
  | .int _ => .int | .bigInt _ => .bigInt | .byte _ => .byte | .decimal _ => .decimal
  | .float _ => .float | .bool _ => .bool | .char _ => .char | .text _ => .text
  | .bytes _ => .bytes | .unit => .unit | .time _ => .time | .duration _ => .duration

/-! ## Primitive operations

Each primitive has a typing signature, `Prim.sig`: its argument types and its
result type. Primitives are first order; library functions that take
functions, such as `map`, `filter` and `sort_by`, are Core functions
(`MoCore/Lib.lean`). Every primitive checks its own bugs (reference 13.2):
overflow, division by zero, and calls outside what it accepts stop the
program. -/

/-- Operations on `Int`. -/
inductive IntOp where
  | add | sub | mul | div | rem | neg | abs | min | max | pow | floorDiv | floorMod
  | checkedAdd | checkedSub | checkedMul
  | wrappingAdd | wrappingSub | wrappingMul
  | saturatingAdd | saturatingSub | saturatingMul
  | toText | toDecimal | toBig | toFloat | toChar | toByte
  | seconds | milliseconds | minutes | hours | days
  deriving DecidableEq, Repr, Inhabited

/-- Operations on `BigInt`. -/
inductive BigOp where
  | add | sub | mul | div | rem | neg | abs | min | max | pow | floorDiv | floorMod
  | toInt | toText
  deriving DecidableEq, Repr, Inhabited

/-- Operations on `Byte`, which has no arithmetic (reference 3.2): a program
converts a byte with `to_int` first. -/
inductive ByteOp where
  | toInt | toText
  deriving DecidableEq, Repr, Inhabited

/-- Operations on `Decimal`. -/
inductive DecOp where
  | add | sub | mul | div | rem | neg | abs | min | max | floor | ceil | round
  | toInt | toText | toFixed | toFloat
  deriving DecidableEq, Repr, Inhabited

/-- Operations on `Float` (IEEE 754; not part of the safety proofs' interest,
but typed like every other primitive). -/
inductive FloatOp where
  | add | sub | mul | div | rem | neg | abs | sqrt | floor | ceil | round
  | isNan | isInfinite | toDecimal | toText
  deriving DecidableEq, Repr, Inhabited

/-- The types whose order is built in. -/
inductive Base where
  | int | bigInt | byte | decimal | float | bool | char | text | bytes | time | duration
  deriving DecidableEq, Repr, Inhabited

/-- The type a `Base` stands for. -/
def Base.ty : Base → Ty
  | .int => .int | .bigInt => .bigInt | .byte => .byte | .decimal => .decimal | .float => .float
  | .bool => .bool | .char => .char | .text => .text | .bytes => .bytes | .time => .time
  | .duration => .duration

/-- Order comparisons. Equality is the structural `Prim.eq`. -/
inductive CmpOp where
  | lt | le | gt | ge
  deriving DecidableEq, Repr, Inhabited

/-- Operations on `Char`. -/
inductive CharOp where
  | code | isDigit | isLetter | isWhiteSpace | lower | upper | toText
  deriving DecidableEq, Repr, Inhabited

/-- Operations on `Text`. -/
inductive TextOp where
  | concat | byteCount | charCount | isEmpty | chars | fromChars | lines | words
  | split | trim | trimStart | trimEnd | startsWith | endsWith | contains
  | dropPrefix | dropSuffix | replace | «repeat» | padLeft | padRight | lower | upper
  | toInt | toDecimal | toFloat | join
  /-- `text.bytes`: the text's UTF-8 encoding. -/
  | bytes
  deriving DecidableEq, Repr, Inhabited

/-- Operations on `Bytes`. `decodeUtf8` gives `None` for bytes that aren't
valid UTF-8 (reference 3.4). -/
inductive BytesOp where
  | size | get | decodeUtf8
  deriving DecidableEq, Repr, Inhabited

/-- Unicode 16.0 trusted primitives; Core specifies their interface, not tables. -/
inductive UnicodeOp where
  | graphemes | displayWidth | normalizeNfc | lowercase | uppercase
  deriving DecidableEq, Repr, Inhabited

/-- Operations on `Range`. `make a b` is `a..b`, the whole numbers from `a` up
to, not including, `b` (reference 7.5); `start` and `end` read its ends, which
is how a `for` loop takes a range apart, as `uncons` takes a list apart. -/
inductive RangeOp where
  | make | start | «end» | toList | contains | toText
  deriving DecidableEq, Repr, Inhabited

/-- Operations on `Time` and `Duration` (reference 3.9). `sub` is `Time - Time`,
a `Duration`; `add` and `subDur` are `Time + Duration` and `Time - Duration`,
each a bug outside the years 1 to 9999; `checkedAdd` is `checked_add`, which
gives `None` there instead. -/
inductive TimeOp where
  | sub | add | subDur | checkedAdd | toText | parse
  | durAdd | durSub | durMul | durNeg | inMilliseconds | inSeconds | durToText
  deriving DecidableEq, Repr, Inhabited

/-- Operations on `List(T)`, at one element type. `lit n` builds a list from
`n` values; `uncons` is Core's way to take a list apart in loops. -/
inductive ListOp where
  | lit (n : Nat)
  | size | isEmpty | get | first | last | append | prepend | concat | reverse
  | take | drop | set | removeAt | uncons | enumerate | contains | unique
  | zip (u : Ty)
  deriving Repr, Inhabited

/-- Operations on `Map(K, V)`, which keeps insertion order. `lit n` is a map
literal with `n` entries, given as key, value, key, value, ... in source
order; it stops if two keys turn out equal (reference 3.8). `ofList` is
`List.to_map`, in which the later value of a repeated key wins (reference 14). -/
inductive MapOp where
  | lit (n : Nat)
  | empty | size | isEmpty | get | has | set | remove | keys | values | entries | ofList
  deriving DecidableEq, Repr, Inhabited

/-- Operations on `Set(T)`, which keeps insertion order and holds no element
twice (reference 3.8). `empty` is the constant `Set.empty`; `ofList` is
`List.to_set`; `toList` is `to_list`, which a `for` loop takes apart as it
takes a list apart. -/
inductive SetOp where
  | empty | size | isEmpty | has | add | remove | union | intersection | difference
  | toList | ofList
  deriving DecidableEq, Repr, Inhabited

/-- Operations on capabilities: the only way a program touches the world.
`ro` selects `ReadFiles` (true) or `Files` (false). -/
inductive IoOp where
  | argsList | envGet
  | print | printError | readLine
  /-- What a fake console has printed: only tests, examples and promises use
  these (reference 12.4). -/
  | printed | printedErrors
  | readText (ro : Bool)
  /-- `read_bytes`: the world keeps each file as its text, so a file's bytes
  are its text's UTF-8 encoding. -/
  | readBytes (ro : Bool)
  | writeText | appendText | remove
  | list (ro : Bool) | entries (ro : Bool) | «exists» (ro : Bool) | within (ro : Bool) | readOnly
  | now | randomInt
  /-- Fake constructors: only spec code may allocate these handles. -/
  | consoleFake | consoleFakeInput | filesInMemory | clockFixed | randomSeeded | argsOf | envOf
  deriving DecidableEq, Repr, Inhabited

/-- The primitive operations. -/
inductive Prim where
  | int (op : IntOp)
  | big (op : BigOp)
  | byte (op : ByteOp)
  | dec (op : DecOp)
  | float (op : FloatOp)
  /-- Boolean negation (`and`, `or` and `implies` lower to `if`). -/
  | not
  | cmp (b : Base) (op : CmpOp)
  | char (op : CharOp)
  | text (op : TextOp)
  | bytes (op : BytesOp)
  | unicode16 (op : UnicodeOp)
  | range (op : RangeOp)
  | time (op : TimeOp)
  | list (t : Ty) (op : ListOp)
  | map (k v : Ty) (op : MapOp)
  | set (t : Ty) (op : SetOp)
  /-- Structural equality at a type with the `Equal` ability (reference 9.2):
  fields compared in turn, maps by their entries and sets by their elements in
  any order, floats by IEEE. -/
  | eq (t : Ty)
  | io (op : IoOp)
  deriving Repr, Inhabited

/-- The capability type of `Files` (`ro = false`) or `ReadFiles` (`ro = true`). -/
def filesCap (ro : Bool) : Ty := .cap (if ro then .readFiles else .files)

def IntOp.sig : IntOp → List Ty × Ty
  | .add | .sub | .mul | .div | .rem | .min | .max | .pow | .floorDiv | .floorMod
  | .wrappingAdd | .wrappingSub | .wrappingMul
  | .saturatingAdd | .saturatingSub | .saturatingMul => ([.int, .int], .int)
  | .neg | .abs => ([.int], .int)
  | .checkedAdd | .checkedSub | .checkedMul => ([.int, .int], .option .int)
  | .toText => ([.int], .text)
  | .toDecimal => ([.int], .decimal)
  | .toBig => ([.int], .bigInt)
  | .toFloat => ([.int], .float)
  | .toChar => ([.int], .option .char)
  | .toByte => ([.int], .option .byte)
  | .seconds | .milliseconds | .minutes | .hours | .days => ([.int], .duration)

def BigOp.sig : BigOp → List Ty × Ty
  | .add | .sub | .mul | .div | .rem | .min | .max | .floorDiv | .floorMod => ([.bigInt, .bigInt], .bigInt)
  | .pow => ([.bigInt, .int], .bigInt)
  | .neg | .abs => ([.bigInt], .bigInt)
  | .toInt => ([.bigInt], .option .int)
  | .toText => ([.bigInt], .text)

def ByteOp.sig : ByteOp → List Ty × Ty
  | .toInt => ([.byte], .int)
  | .toText => ([.byte], .text)

def DecOp.sig : DecOp → List Ty × Ty
  | .add | .sub | .mul | .div | .rem | .min | .max => ([.decimal, .decimal], .decimal)
  | .neg | .abs | .floor | .ceil => ([.decimal], .decimal)
  | .round => ([.decimal, .int], .decimal)
  | .toInt => ([.decimal], .option .int)
  | .toText => ([.decimal], .text)
  | .toFixed => ([.decimal, .int], .text)
  | .toFloat => ([.decimal], .float)

def FloatOp.sig : FloatOp → List Ty × Ty
  | .add | .sub | .mul | .div | .rem => ([.float, .float], .float)
  | .neg | .abs | .sqrt | .floor | .ceil | .round => ([.float], .float)
  | .isNan | .isInfinite => ([.float], .bool)
  | .toDecimal => ([.float], .option .decimal)
  | .toText => ([.float], .text)

def CharOp.sig : CharOp → List Ty × Ty
  | .code => ([.char], .int)
  | .isDigit | .isLetter | .isWhiteSpace => ([.char], .bool)
  | .lower | .upper => ([.char], .char)
  | .toText => ([.char], .text)

def TextOp.sig : TextOp → List Ty × Ty
  | .concat => ([.text, .text], .text)
  | .byteCount | .charCount => ([.text], .int)
  | .isEmpty => ([.text], .bool)
  | .chars => ([.text], .list .char)
  | .fromChars => ([.list .char], .text)
  | .lines | .words => ([.text], .list .text)
  | .split => ([.text, .text], .list .text)
  | .trim | .trimStart | .trimEnd | .lower | .upper => ([.text], .text)
  | .startsWith | .endsWith | .contains => ([.text, .text], .bool)
  | .dropPrefix | .dropSuffix => ([.text, .text], .option .text)
  | .replace => ([.text, .text, .text], .text)
  | .repeat | .padLeft | .padRight => ([.text, .int], .text)
  | .toInt => ([.text], .option .int)
  | .toDecimal => ([.text], .option .decimal)
  | .toFloat => ([.text], .option .float)
  | .join => ([.list .text, .text], .text)
  | .bytes => ([.text], .bytes)

def BytesOp.sig : BytesOp → List Ty × Ty
  | .size => ([.bytes], .int)
  | .get => ([.bytes, .int], .option .byte)
  | .decodeUtf8 => ([.bytes], .option .text)

def RangeOp.sig : RangeOp → List Ty × Ty
  | .make => ([.int, .int], .range)
  | .start | .end => ([.range], .int)
  | .toList => ([.range], .list .int)
  | .contains => ([.range, .int], .bool)
  | .toText => ([.range], .text)

def TimeOp.sig : TimeOp → List Ty × Ty
  | .sub => ([.time, .time], .duration)
  | .add | .subDur => ([.time, .duration], .time)
  | .checkedAdd => ([.time, .duration], .option .time)
  | .toText => ([.time], .text)
  | .parse => ([.text], .option .time)
  | .durAdd | .durSub => ([.duration, .duration], .duration)
  | .durMul => ([.duration, .int], .duration)
  | .durNeg => ([.duration], .duration)
  | .inMilliseconds => ([.duration], .int)
  | .inSeconds => ([.duration], .decimal)
  | .durToText => ([.duration], .text)

def ListOp.sig (t : Ty) : ListOp → List Ty × Ty
  | .lit n => (List.replicate n t, .list t)
  | .size => ([.list t], .int)
  | .isEmpty => ([.list t], .bool)
  | .get => ([.list t, .int], .option t)
  | .first | .last => ([.list t], .option t)
  | .append | .prepend => ([.list t, t], .list t)
  | .concat => ([.list t, .list t], .list t)
  | .reverse | .unique => ([.list t], .list t)
  | .take | .drop => ([.list t, .int], .list t)
  | .set => ([.list t, .int, t], .option (.list t))
  | .removeAt => ([.list t, .int], .option (.list t))
  | .uncons => ([.list t], .option (.tuple [t, .list t]))
  | .enumerate => ([.list t], .list (.tuple [.int, t]))
  | .contains => ([.list t, t], .bool)
  | .zip u => ([.list t, .list u], .list (.tuple [t, u]))

/-- The argument types of a map literal with `n` entries: a key and its value
for each entry. -/
def mapLitTys (k v : Ty) : Nat → List Ty
  | 0 => []
  | n + 1 => k :: v :: mapLitTys k v n

def MapOp.sig (k v : Ty) : MapOp → List Ty × Ty
  | .lit n => (mapLitTys k v n, .map k v)
  | .empty => ([], .map k v)
  | .size => ([.map k v], .int)
  | .isEmpty => ([.map k v], .bool)
  | .get => ([.map k v, k], .option v)
  | .has => ([.map k v, k], .bool)
  | .set => ([.map k v, k, v], .map k v)
  | .remove => ([.map k v, k], .map k v)
  | .keys => ([.map k v], .list k)
  | .values => ([.map k v], .list v)
  | .entries => ([.map k v], .list (.tuple [k, v]))
  | .ofList => ([.list (.tuple [k, v])], .map k v)

def SetOp.sig (t : Ty) : SetOp → List Ty × Ty
  | .empty => ([], .set t)
  | .size => ([.set t], .int)
  | .isEmpty => ([.set t], .bool)
  | .has => ([.set t, t], .bool)
  | .add | .remove => ([.set t, t], .set t)
  | .union | .intersection | .difference => ([.set t, .set t], .set t)
  | .toList => ([.set t], .list t)
  | .ofList => ([.list t], .set t)

def UnicodeOp.sig : UnicodeOp → List Ty × Ty
  | .graphemes => ([.text], .list .text)
  | .displayWidth => ([.text], .int)
  | .normalizeNfc | .lowercase | .uppercase => ([.text], .text)

def IoOp.sig : IoOp → List Ty × Ty
  | .argsList => ([.cap .args], .list .text)
  | .envGet => ([.cap .env, .text], .option .text)
  | .print | .printError => ([.cap .console, .text], .unit)
  | .readLine => ([.cap .console], .option .text)
  | .printed | .printedErrors => ([.cap .console], .list .text)
  | .readText ro => ([filesCap ro, .text], .result .text .fileError)
  | .readBytes ro => ([filesCap ro, .text], .result .bytes .fileError)
  | .writeText | .appendText => ([.cap .files, .text, .text], .result .unit .fileError)
  | .remove => ([.cap .files, .text], .result .unit .fileError)
  | .list ro => ([filesCap ro, .text], .result (.list .text) .fileError)
  | .entries ro => ([filesCap ro, .text], .result (.list .entry) .fileError)
  | .exists ro => ([filesCap ro, .text], .bool)
  | .within ro => ([filesCap ro, .text], filesCap ro)
  | .readOnly => ([.cap .files], .cap .readFiles)
  | .now => ([.cap .clock], .time)
  | .randomInt => ([.cap .random, .int, .int], .int)
  | .consoleFake => ([], .cap .console)
  | .consoleFakeInput => ([.list .text], .cap .console)
  | .filesInMemory => ([.map .text .text], .cap .files)
  | .clockFixed => ([.time], .cap .clock)
  | .randomSeeded => ([.int], .cap .random)
  | .argsOf => ([.list .text], .cap .args)
  | .envOf => ([.map .text .text], .cap .env)

/-- The typing signature of every primitive: argument types and result type. -/
def Prim.sig : Prim → List Ty × Ty
  | .int op => op.sig
  | .big op => op.sig
  | .byte op => op.sig
  | .dec op => op.sig
  | .float op => op.sig
  | .not => ([.bool], .bool)
  | .cmp b _ => ([b.ty, b.ty], .bool)
  | .char op => op.sig
  | .text op => op.sig
  | .bytes op => op.sig
  | .unicode16 op => op.sig
  | .range op => op.sig
  | .time op => op.sig
  | .list t op => op.sig t
  | .map k v op => op.sig k v
  | .set t op => op.sig t
  | .eq t => ([t, t], .bool)
  | .io op => op.sig

/-! ## Expressions -/

/-- Core expressions.

* `case_ e arms` takes apart one level of a constructed value: there is one
  arm per constructor of the scrutinee's type, in declaration order, each
  naming the constructor and binding its fields to variables.
* `join j params body rest` declares a join point: a local continuation `j`
  whose body runs, with `params` bound, when `rest` (or the body itself) jumps
  to it with `jump j args`. Jumps are allowed only in tail position (enforced
  by the typing judgment), and a join body may jump to its own join point:
  that is how loops, `break`, `continue`, `return` and `try` lower. -/
inductive Expr where
  | lit (l : Lit)
  | var (x : String)
  /-- A module constant; separate from local variables, so it cannot be shadowed. -/
  | global (name : String)
  | let_ (x : String) (e : Expr) (body : Expr)
  /-- An anonymous function; it captures its environment when evaluated. -/
  | lam (params : List (String × Ty)) (body : Expr)
  /-- Application of a function value. -/
  | app (f : Expr) (args : List Expr)
  /-- A call of a top-level function. -/
  | call (f : String) (args : List Expr)
  | tuple (es : List Expr)
  /-- `e.i`, the `i`-th part of a tuple, from 0. -/
  | proj (e : Expr) (i : Nat)
  /-- Constructor `c` of the data type `τ`, applied to its fields in order. -/
  | con (τ : Ty) (c : String) (args : List Expr)
  | case_ (e : Expr) (arms : List (String × List String × Expr))
  | if_ (c t f : Expr)
  | prim (p : Prim) (args : List Expr)
  /-- `stop "reason"`: a bug, reported with its reason. -/
  | stop (reason : String)
  /-- An assertion or example failed, with a computed text detail. -/
  | fail (detail : Expr)
  | join (j : String) (params : List (String × Ty)) (body : Expr) (rest : Expr)
  | jump (j : String) (args : List Expr)
  deriving Repr, Inhabited

/-! ## Programs -/

/-- A constructor: its name and its named, typed fields. -/
structure Ctor where
  name : String
  fields : List (String × Ty)
  deriving Repr, Inhabited

/-- A declared data type: a record (one constructor named like the type) or a
choice type (one constructor per case). Monomorphic: `Column(Product)` is
declared as its own type. -/
structure DataDecl where
  name : String
  ctors : List Ctor
  deriving Repr, Inhabited

/-- The four specification kinds and ordinary program code. -/
inductive FunKind where
  | function | test | example | promise | anchor
  deriving Repr, Inhabited, DecidableEq

def FunKind.isSpec : FunKind → Bool
  | .function => false
  | _ => true

/-- The signatures required by bytecode 3.3. A promise has a final result parameter. -/
def FunKind.validSig (kind : FunKind) (ps : List (String × Ty)) (ret : Ty) : Bool :=
  match kind with
  | .function => true
  | .test | .example => ps.isEmpty && ret == .unit
  | .promise => !ps.isEmpty && ret == .int
  | .anchor => ps.isEmpty && ret == .int

/-- A top-level function. `constCount` is the prefix of constants its body
needs, including through calls; lowering's dependency analysis supplies it. -/
structure FunDecl where
  name : String
  params : List (String × Ty)
  ret : Ty
  body : Expr
  module : String := "Main"
  kind : FunKind := .function
  constCount : Nat := 0
  deriving Repr, Inhabited

/-- A module constant, already ordered by lowering's dependency analysis. -/
structure ConstDecl where
  name : String
  ty : Ty
  body : Expr
  deriving Repr, Inhabited

/-- Compiler-only side tables. None of these fields is read by typing or evaluation. -/
structure SourceSpan where
  file : Nat
  start : Nat
  «end» : Nat
  deriving Repr, Inhabited, BEq

/-- A source binding at a preorder node and binder slot; parameters live in BodyMeta. -/
structure BindingMeta where
  node : Nat
  slot : Nat
  name : String
  deriving Repr, Inhabited, BEq

structure BodyMeta where
  spans : List (Option SourceSpan)
  params : List (Option String)
  bindings : List BindingMeta
  closures : List (Nat × String)
  deriving Repr, Inhabited, BEq

structure SpecLine where
  span : Option SourceSpan
  text : String
  deriving Repr, Inhabited, BEq

structure FunMeta where
  name : String
  span : Option SourceSpan
  text : Option String
  body : BodyMeta
  subject : Option Nat
  allows : Option Bool
  lines : List SpecLine
  deriving Repr, Inhabited, BEq

structure ConstMeta where
  name : String
  span : Option SourceSpan
  body : BodyMeta
  deriving Repr, Inhabited, BEq

structure ModuleMeta where
  name : String
  data : List Nat
  funs : List Nat
  consts : List Nat
  exposedTypes : List (Nat × Bool)
  exposedFuns : List Nat
  deriving Repr, Inhabited, BEq

inductive CompilerMeta where
  | noSource
  | source (files : List String) (modules : List ModuleMeta)
      (funs : List FunMeta) (consts : List ConstMeta)
  deriving Repr, Inhabited, BEq

/-- A whole Core program, with one designated entry function. -/
structure Program where
  data : List DataDecl
  funs : List FunDecl
  consts : List ConstDecl := []
  entry : String := "main"
  metadata : CompilerMeta := .noSource
  deriving Repr, Inhabited

/-- The types of the first `n` constants, in computation order. -/
def constCtx (P : Program) (n : Nat) : List (String × Ty) :=
  (P.consts.take n).map fun c => (c.name, c.ty)

/-- Fake construction and console observations belong only to spec code. -/
def Prim.specOnly : Prim → Bool
  | .io .printed | .io .printedErrors => true
  | .io .consoleFake | .io .consoleFakeInput | .io .filesInMemory | .io .clockFixed
  | .io .randomSeeded | .io .argsOf | .io .envOf => true
  | _ => false

/-! ## Lookups -/

/-- The first binding of a name in an association list. -/
def lookup {β : Type} (x : String) : List (String × β) → Option β
  | [] => none
  | (y, b) :: rest => if x = y then some b else lookup x rest

/-- The fields of the first constructor named `c`. -/
def ctorFields (c : String) : List Ctor → Option (List (String × Ty))
  | [] => none
  | k :: ks => if c = k.name then some k.fields else ctorFields c ks

/-- The first data declaration with a name. -/
def findData (s : String) : List DataDecl → Option DataDecl
  | [] => none
  | d :: ds => if s = d.name then some d else findData s ds

/-- The first function with a name. -/
def lookupFun (P : Program) (f : String) : Option FunDecl :=
  go P.funs
where
  go : List FunDecl → Option FunDecl
    | [] => none
    | fd :: fds => if f = fd.name then some fd else go fds

theorem lookupFun_go_mem {f : String} : ∀ {fds : List FunDecl} {fd : FunDecl},
    lookupFun.go f fds = some fd → fd ∈ fds
  | [], _, h => by simp [lookupFun.go] at h
  | g :: gs, fd, h => by
    simp only [lookupFun.go] at h
    split at h
    · simp only [Option.some.injEq] at h; subst h; simp
    · exact List.mem_cons_of_mem _ (lookupFun_go_mem h)

theorem lookupFun_mem {P : Program} {f : String} {fd : FunDecl}
    (h : lookupFun P f = some fd) : fd ∈ P.funs := lookupFun_go_mem h

/-- The constructors of `Option(T)`: `None` then `Some(T)`, in the order
reference 8 declares them, which is also their order (reference 9.2). -/
def optionCtors (t : Ty) : List Ctor := [⟨"None", []⟩, ⟨"Some", [("value", t)]⟩]

/-- The constructors of `Result(T, E)`: `Ok(T)` then `Error(E)`. -/
def resultCtors (t e : Ty) : List Ctor := [⟨"Ok", [("value", t)]⟩, ⟨"Error", [("error", e)]⟩]

/-- The constructors of `Ordering` (reference 9.2). -/
def orderingCtors : List Ctor := [⟨"Less", []⟩, ⟨"Equal", []⟩, ⟨"Greater", []⟩]

/-- The constructors of `FileError` (reference 14). -/
def fileErrorCtors : List Ctor :=
  [⟨"NotFound", [("path", .text)]⟩, ⟨"NotAllowed", [("path", .text)]⟩,
   ⟨"NotText", [("path", .text)]⟩, ⟨"Failed", [("path", .text), ("reason", .text)]⟩]

/-- The constructors of a type that has them: declared data types, and the
library's always-in-scope choice types. -/
def ctorsOf (P : Program) : Ty → Option (List Ctor)
  | .data s => (findData s P.data).map (·.ctors)
  | .option t => some (optionCtors t)
  | .result t e => some (resultCtors t e)
  | .ordering => some orderingCtors
  | .fileError => some fileErrorCtors
  | .entry => some [⟨"Entry", [("name", .text), ("kind", .entryKind)]⟩]
  | .entryKind => some [⟨"File", []⟩, ⟨"Folder", []⟩, ⟨"Link", []⟩, ⟨"Other", []⟩]
  | _ => none

end MoCore
