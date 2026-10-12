/-
  Mo Core: what the primitives do.

  `Prim.eval p vs w` applies primitive `p` to argument values `vs` in world
  `w`. It returns `ok`, `stopped` on a bug, or `wrong` when the arguments are
  not of the kinds the signature promises (which typing rules out). Every
  number a primitive makes goes through a range check (`okInt`, `okDec`,
  `okDur`, `okTime`, and the `opt...` forms for results that are `Option`s), so
  arithmetic overflow stops instead of wrapping (reference 13.2).
-/
import MoCore.Support.Time
import MoCore.Support.Equiv

namespace MoCore

/-! ## Results -/

def okInt (n : Int) (w : World) : Outcome :=
  if InInt64 n then .ok (.int n) w else .stopped (.overflow "Int") w

def okDec (n : Int) (w : World) : Outcome :=
  if InDecimal n then .ok (.decimal n) w else .stopped (.overflow "Decimal") w

def okDur (n : Int) (w : World) : Outcome :=
  if InInt64 n then .ok (.duration n) w else .stopped (.overflow "Duration") w

def okTime (t : Int) (w : World) : Outcome :=
  if InTime t then .ok (.time t) w else .stopped (.overflow "a time outside the years 1 to 9999") w

/-- `Some(n)` when `n` fits in an `Int`, else `None`. -/
def optInt (n : Int) : Value := if InInt64 n then someV (.int n) else noneV

/-- `Some(n)` when `n` is a `Byte`, from 0 to 255, else `None`. -/
def optByte (n : Int) : Value := if InByte n then someV (.byte n) else noneV

/-- `Some(n)` when `n` fits in a `Decimal`, else `None`. -/
def optDec (n : Int) : Value := if InDecimal n then someV (.decimal n) else noneV

/-- `Some(t)` when `t` is between the years 1 and 9999, else `None`. -/
def optTime (t : Int) : Value := if InTime t then someV (.time t) else noneV

/-- `Some(v)` or `None`. -/
def optOf : Option Value → Value
  | some v => someV v
  | none => noneV

/-- An optional `Int` from a parser, range-checked. -/
def optIntOf : Option Int → Value
  | some n => optInt n
  | none => noneV

/-- An optional `Decimal` from a parser, range-checked. -/
def optDecOf : Option Int → Value
  | some n => optDec n
  | none => noneV

/-- An optional `Time` from a parser, range-checked. -/
def optTimeOf : Option Int → Value
  | some t => optTime t
  | none => noneV

/-- An optional `Text`. -/
def optText : Option String → Value
  | some s => someV (.text s)
  | none => noneV

/-- An optional `Char`. -/
def optChar : Option Char → Value
  | some c => someV (.char c)
  | none => noneV

/-- A bug: a library call outside what it accepts. -/
def outside (what : String) (w : World) : Outcome := .stopped (.outside what) w

/-- Arguments of the wrong kinds: impossible for well-typed programs. -/
def wrongArgs (what : String) : Outcome := .wrong s!"{what}: arguments of the wrong kinds"

/-! ## Numbers -/

def evalIntOp (op : IntOp) (vs : List Value) (w : World) : Outcome :=
  match op, vs with
  | .add, [.int a, .int b] => okInt (a + b) w
  | .sub, [.int a, .int b] => okInt (a - b) w
  | .mul, [.int a, .int b] => okInt (a * b) w
  | .div, [.int a, .int b] => if b = 0 then .stopped .divByZero w else okInt (a.tdiv b) w
  | .rem, [.int a, .int b] => if b = 0 then .stopped .divByZero w else okInt (a.tmod b) w
  | .neg, [.int a] => okInt (-a) w
  | .abs, [.int a] => okInt a.natAbs w
  | .min, [.int a, .int b] => okInt (min a b) w
  | .max, [.int a, .int b] => okInt (max a b) w
  | .pow, [.int a, .int n] =>
    if n < 0 then outside "Int.pow with a negative exponent" w else okInt (a ^ n.toNat) w
  | .floorDiv, [.int a, .int b] => if b = 0 then .stopped .divByZero w else okInt (a.fdiv b) w
  | .floorMod, [.int a, .int b] => if b = 0 then .stopped .divByZero w else okInt (a.fmod b) w
  | .checkedAdd, [.int a, .int b] => .ok (optInt (a + b)) w
  | .checkedSub, [.int a, .int b] => .ok (optInt (a - b)) w
  | .checkedMul, [.int a, .int b] => .ok (optInt (a * b)) w
  | .wrappingAdd, [.int a, .int b] => okInt (wrap64 (a + b)) w
  | .wrappingSub, [.int a, .int b] => okInt (wrap64 (a - b)) w
  | .wrappingMul, [.int a, .int b] => okInt (wrap64 (a * b)) w
  | .saturatingAdd, [.int a, .int b] => okInt (saturate64 (a + b)) w
  | .saturatingSub, [.int a, .int b] => okInt (saturate64 (a - b)) w
  | .saturatingMul, [.int a, .int b] => okInt (saturate64 (a * b)) w
  | .toText, [.int a] => .ok (.text (toString a)) w
  | .toDecimal, [.int a] => okDec (a * decScale) w
  | .toBig, [.int a] => .ok (.bigInt a) w
  | .toFloat, [.int a] => .ok (.float (Float.ofInt a).toBits) w
  | .toChar, [.int a] => .ok (optChar (charOfCode a)) w
  | .toByte, [.int a] => .ok (optByte a) w
  | .seconds, [.int a] => okDur (a * 1000000) w
  | .milliseconds, [.int a] => okDur (a * 1000) w
  | .minutes, [.int a] => okDur (a * 60000000) w
  | .hours, [.int a] => okDur (a * 3600000000) w
  | .days, [.int a] => okDur (a * 86400000000) w
  | _, _ => wrongArgs "Int primitive"

def evalBigOp (op : BigOp) (vs : List Value) (w : World) : Outcome :=
  match op, vs with
  | .add, [.bigInt a, .bigInt b] => .ok (.bigInt (a + b)) w
  | .sub, [.bigInt a, .bigInt b] => .ok (.bigInt (a - b)) w
  | .mul, [.bigInt a, .bigInt b] => .ok (.bigInt (a * b)) w
  | .div, [.bigInt a, .bigInt b] => if b = 0 then .stopped .divByZero w else .ok (.bigInt (a.tdiv b)) w
  | .rem, [.bigInt a, .bigInt b] => if b = 0 then .stopped .divByZero w else .ok (.bigInt (a.tmod b)) w
  | .neg, [.bigInt a] => .ok (.bigInt (-a)) w
  | .abs, [.bigInt a] => .ok (.bigInt a.natAbs) w
  | .toInt, [.bigInt a] => .ok (optInt a) w
  | .toText, [.bigInt a] => .ok (.text (toString a)) w
  | _, _ => wrongArgs "BigInt primitive"

/-- `Byte.to_int` always fits; a byte's text form is its decimal digits, as an
`Int`'s is. -/
def evalByteOp (op : ByteOp) (vs : List Value) (w : World) : Outcome :=
  match op, vs with
  | .toInt, [.byte a] => okInt a w
  | .toText, [.byte a] => .ok (.text (toString a)) w
  | _, _ => wrongArgs "Byte primitive"

/-- `Decimal.to_int`: `None` unless whole and in the `Int` range. -/
def decToIntV (a : Int) : Value := if a % decScale = 0 then optInt (a / decScale) else noneV

/-- Places for `round` and `to_fixed`: 0 to 18. -/
def placesOk (p : Int) : Bool := 0 ≤ p && p ≤ 18

def evalDecOp (op : DecOp) (vs : List Value) (w : World) : Outcome :=
  match op, vs with
  | .add, [.decimal a, .decimal b] => okDec (a + b) w
  | .sub, [.decimal a, .decimal b] => okDec (a - b) w
  | .mul, [.decimal a, .decimal b] => okDec (decMul a b) w
  | .div, [.decimal a, .decimal b] => if b = 0 then .stopped .divByZero w else okDec (decDiv a b) w
  | .rem, [.decimal a, .decimal b] => if b = 0 then .stopped .divByZero w else okDec (a.tmod b) w
  | .min, [.decimal a, .decimal b] => okDec (min a b) w
  | .max, [.decimal a, .decimal b] => okDec (max a b) w
  | .neg, [.decimal a] => okDec (-a) w
  | .abs, [.decimal a] => okDec a.natAbs w
  | .floor, [.decimal a] => okDec (decFloor a) w
  | .ceil, [.decimal a] => okDec (decCeil a) w
  | .round, [.decimal a, .int p] =>
    if placesOk p then okDec (decRound a p.toNat) w
    else outside "Decimal.round with places outside 0 to 18" w
  | .toInt, [.decimal a] => .ok (decToIntV a) w
  | .toText, [.decimal a] => .ok (.text (decToText a)) w
  | .toFixed, [.decimal a, .int p] =>
    if placesOk p then .ok (.text (decToFixed a p.toNat)) w
    else outside "Decimal.to_fixed with places outside 0 to 18" w
  | .toFloat, [.decimal a] => .ok (.float (decToFloat a).toBits) w
  | _, _ => wrongArgs "Decimal primitive"

/-- A float's text form. A float that is not a number is `nan`, and the
infinities are `inf` and `-inf` (reference 9.2). Any other float is Lean's
`toString`, not the shortest spelling the reference asks for: the VM's is a
trusted primitive, checked against Ryu (architecture 4.5). -/
def floatToText (x : Float) : String :=
  if x.isNaN then "nan" else if x.isInf then (if x < 0 then "-inf" else "inf") else toString x

def evalFloatOp (op : FloatOp) (vs : List Value) (w : World) : Outcome :=
  let f := Float.ofBits
  let ret (x : Float) : Outcome := .ok (.float x.toBits) w
  match op, vs with
  | .add, [.float a, .float b] => ret (f a + f b)
  | .sub, [.float a, .float b] => ret (f a - f b)
  | .mul, [.float a, .float b] => ret (f a * f b)
  | .div, [.float a, .float b] => ret (f a / f b)
  | .neg, [.float a] => ret (-(f a))
  | .abs, [.float a] => ret (f a).abs
  | .sqrt, [.float a] => ret (f a).sqrt
  | .floor, [.float a] => ret (f a).floor
  | .ceil, [.float a] => ret (f a).ceil
  | .round, [.float a] => ret (f a).round
  | .isNan, [.float a] => .ok (.bool (f a).isNaN) w
  | .isInfinite, [.float a] => .ok (.bool (f a).isInf) w
  | .toText, [.float a] => .ok (.text (floatToText (f a))) w
  | _, _ => wrongArgs "Float primitive"

/-- `<`, `<=`, `>` and `>=` at a base type. -/
def evalCmp (b : Base) (op : CmpOp) (vs : List Value) (w : World) : Outcome :=
  match b, vs with
  | .float, [.float x, .float y] => .ok (.bool (floatCmp op (Float.ofBits x) (Float.ofBits y))) w
  | b, [x, y] =>
    match baseOrder b x y with
    | some o => .ok (.bool (op.holds o)) w
    | none => wrongArgs "comparison"
  | _, _ => wrongArgs "comparison"

/-! ## Characters and text -/

def evalCharOp (op : CharOp) (vs : List Value) (w : World) : Outcome :=
  match op, vs with
  | .code, [.char c] => okInt c.toNat w
  | .isDigit, [.char c] => .ok (.bool (isDigitChar c)) w
  | .isLetter, [.char c] => .ok (.bool (isAsciiLetter c)) w
  | .isWhiteSpace, [.char c] => .ok (.bool (isAsciiWhite c)) w
  | .lower, [.char c] => .ok (.char (asciiLowerChar c)) w
  | .upper, [.char c] => .ok (.char (asciiUpperChar c)) w
  | .toText, [.char c] => .ok (.text (String.singleton c)) w
  | _, _ => wrongArgs "Char primitive"

/-- The characters of a list of `Char` values. -/
def charsOf : List Value → Option (List Char)
  | [] => some []
  | .char c :: vs => (charsOf vs).map (c :: ·)
  | _ => none

/-- The texts of a list of `Text` values. -/
def textsOf : List Value → Option (List String)
  | [] => some []
  | .text s :: vs => (textsOf vs).map (s :: ·)
  | _ => none

def texts (xs : List String) : Value := .list (xs.map .text)

def evalTextOp (op : TextOp) (vs : List Value) (w : World) : Outcome :=
  match op, vs with
  | .concat, [.text a, .text b] => .ok (.text (a ++ b)) w
  | .byteCount, [.text a] => okInt a.utf8ByteSize w
  | .charCount, [.text a] => okInt a.length w
  | .isEmpty, [.text a] => .ok (.bool a.isEmpty) w
  | .chars, [.text a] => .ok (.list (a.toList.map .char)) w
  | .fromChars, [.list cs] =>
    match charsOf cs with
    | some l => .ok (.text (String.ofList l)) w
    | none => wrongArgs "Text.from_chars"
  | .lines, [.text a] => .ok (texts (textLines a)) w
  | .words, [.text a] => .ok (texts (textWords a)) w
  | .split, [.text a, .text sep] =>
    if sep.isEmpty then outside "Text.split with an empty separator" w
    else .ok (texts (textSplit a sep)) w
  | .trim, [.text a] => .ok (.text (textTrim a)) w
  | .trimStart, [.text a] => .ok (.text (textTrimStart a)) w
  | .trimEnd, [.text a] => .ok (.text (textTrimEnd a)) w
  | .startsWith, [.text a, .text b] => .ok (.bool (textStartsWith a b)) w
  | .endsWith, [.text a, .text b] => .ok (.bool (textEndsWith a b)) w
  | .contains, [.text a, .text b] => .ok (.bool (textContains a b)) w
  | .dropPrefix, [.text a, .text b] => .ok (optText (textDropPrefix a b)) w
  | .dropSuffix, [.text a, .text b] => .ok (optText (textDropSuffix a b)) w
  | .replace, [.text a, .text old, .text new] =>
    if old.isEmpty then outside "Text.replace of an empty text" w
    else .ok (.text (textReplace a old new)) w
  | .repeat, [.text a, .int n] =>
    if n < 0 then outside "Text.repeat with a negative count" w
    else .ok (.text (textRepeat a n.toNat)) w
  | .padLeft, [.text a, .int n] => .ok (.text (textPadLeft a n)) w
  | .padRight, [.text a, .int n] => .ok (.text (textPadRight a n)) w
  | .lower, [.text a] => .ok (.text (textLower a)) w
  | .upper, [.text a] => .ok (.text (textUpper a)) w
  | .toInt, [.text a] => .ok (optIntOf (parseIntText a)) w
  | .toDecimal, [.text a] => .ok (optDecOf (parseDecimalText a)) w
  | .join, [.list xs, .text sep] =>
    match textsOf xs with
    | some ts => .ok (.text (String.intercalate sep ts)) w
    | none => wrongArgs "List.join"
  | .bytes, [.text a] => .ok (.bytes (textBytes a)) w
  | _, _ => wrongArgs "Text primitive"

/-! ## Bytes -/

/-- `bytes.get(i)`: `None` for a position below 0 or past the end, as for a
list. -/
def bytesGet (bs : List Int) (i : Int) : Value :=
  if 0 ≤ i then optOf (bs[i.toNat]?.map .byte) else noneV

def evalBytesOp (op : BytesOp) (vs : List Value) (w : World) : Outcome :=
  match op, vs with
  | .size, [.bytes bs] => okInt bs.length w
  | .get, [.bytes bs, .int i] => .ok (bytesGet bs i) w
  | .decodeUtf8, [.bytes bs] => .ok (optText ((utf8Decode bs).map String.ofList)) w
  | _, _ => wrongArgs "Bytes primitive"

/-! ## Ranges -/

/-- The whole numbers from `a` up to, not including, `b`. -/
def intRange (a b : Int) : List Value :=
  (List.range (b - a).toNat).map fun (i : Nat) => .int (a + (i : Int))

/-- A range's text form is its ends with `..` between them, as in `0..3`
(reference 9.2). Its `for` loop counts from its start while below its end, so
the counter never passes the end and never overflows (TRANSLATION.md 7.5). -/
def evalRangeOp (op : RangeOp) (vs : List Value) (w : World) : Outcome :=
  match op, vs with
  | .make, [.int a, .int b] => .ok (.range a b) w
  | .start, [.range a _] => .ok (.int a) w
  | .end, [.range _ b] => .ok (.int b) w
  | .toList, [.range a b] => .ok (.list (intRange a b)) w
  | .contains, [.range a b, .int n] => .ok (.bool (a ≤ n && n < b)) w
  | .toText, [.range a b] => .ok (.text (toString a ++ ".." ++ toString b)) w
  | _, _ => wrongArgs "Range primitive"

/-! ## Time and durations -/

def evalTimeOp (op : TimeOp) (vs : List Value) (w : World) : Outcome :=
  match op, vs with
  | .sub, [.time a, .time b] => okDur (a - b) w
  | .add, [.time t, .duration d] => okTime (t + d) w
  | .subDur, [.time t, .duration d] => okTime (t - d) w
  | .checkedAdd, [.time t, .duration d] => .ok (optTime (t + d)) w
  | .toText, [.time t] => .ok (.text (timeToText t)) w
  | .parse, [.text s] => .ok (optTimeOf (parseTime s)) w
  | .durAdd, [.duration a, .duration b] => okDur (a + b) w
  | .durSub, [.duration a, .duration b] => okDur (a - b) w
  | .durMul, [.duration a, .int n] => okDur (a * n) w
  | .durNeg, [.duration a] => okDur (-a) w
  | .inMilliseconds, [.duration a] => okInt (a.tdiv 1000) w
  | .inSeconds, [.duration a] => okDec (a * 1000000000000) w
  | .durToText, [.duration a] => .ok (.text (durationToText a)) w
  | _, _ => wrongArgs "Time primitive"

/-! ## Lists and maps -/

/-- `list.get(i)`: `None` for a position below 0 or past the end. -/
def listGet (xs : List Value) (i : Int) : Value :=
  if 0 ≤ i then optOf xs[i.toNat]? else noneV

/-- `Some((x, rest))` for a list `x :: rest`, `None` for `[]`. -/
def unconsV : List Value → Value
  | [] => noneV
  | x :: rest => someV (.tuple [x, .list rest])

/-- `(i, x)` for each element `x`, counting from `k`. -/
def enumerateVals (k : Nat) : List Value → List Value
  | [] => []
  | x :: xs => .tuple [.int k, x] :: enumerateVals (k + 1) xs

/-- The elements in order, without later duplicates. -/
def uniqueVals (xs : List Value) : List Value :=
  xs.foldl (fun acc x => if acc.any (fun y => Value.equiv y x) then acc else acc ++ [x]) []

def evalListOp (op : ListOp) (vs : List Value) (w : World) : Outcome :=
  match op, vs with
  | .lit _, xs => .ok (.list xs) w
  | .size, [.list xs] => okInt xs.length w
  | .isEmpty, [.list xs] => .ok (.bool xs.isEmpty) w
  | .get, [.list xs, .int i] => .ok (listGet xs i) w
  | .first, [.list xs] => .ok (optOf xs.head?) w
  | .last, [.list xs] => .ok (optOf xs.getLast?) w
  | .append, [.list xs, x] => .ok (.list (xs ++ [x])) w
  | .prepend, [.list xs, x] => .ok (.list (x :: xs)) w
  | .concat, [.list xs, .list ys] => .ok (.list (xs ++ ys)) w
  | .reverse, [.list xs] => .ok (.list xs.reverse) w
  | .take, [.list xs, .int n] =>
    if n < 0 then outside "List.take with a negative count" w else .ok (.list (xs.take n.toNat)) w
  | .drop, [.list xs, .int n] =>
    if n < 0 then outside "List.drop with a negative count" w else .ok (.list (xs.drop n.toNat)) w
  | .set, [.list xs, .int i, x] =>
    .ok (if 0 ≤ i && i < xs.length then someV (.list (xs.set i.toNat x)) else noneV) w
  | .removeAt, [.list xs, .int i] =>
    .ok (if 0 ≤ i && i < xs.length then someV (.list (xs.eraseIdx i.toNat)) else noneV) w
  | .uncons, [.list xs] => .ok (unconsV xs) w
  | .enumerate, [.list xs] =>
    if xs.length ≤ int64Max then .ok (.list (enumerateVals 0 xs)) w
    else .stopped (.overflow "Int") w
  | .contains, [.list xs, x] => .ok (.bool (xs.any fun y => Value.equiv y x)) w
  | .unique, [.list xs] => .ok (.list (uniqueVals xs)) w
  | .zip _, [.list xs, .list ys] => .ok (.list (List.zipWith (fun a b => .tuple [a, b]) xs ys)) w
  | _, _ => wrongArgs "List primitive"

/-- The value at the first key equal to `k`. -/
def mapGet (k : Value) : List (Value × Value) → Option Value
  | [] => none
  | (k', v) :: es => if Value.equiv k' k then some v else mapGet k es

/-- Sets a key: an existing key keeps its place, a new one goes last. -/
def mapSet (es : List (Value × Value)) (k v : Value) : List (Value × Value) :=
  if es.any (fun e => Value.equiv e.1 k) then es.map fun e => if Value.equiv e.1 k then (e.1, v) else e
  else es ++ [(k, v)]

def mapRemove (es : List (Value × Value)) (k : Value) : List (Value × Value) :=
  es.filter fun e => !Value.equiv e.1 k

/-- The pairs of a list of 2-tuples. -/
def pairsOf : List Value → Option (List (Value × Value))
  | [] => some []
  | .tuple [k, v] :: ps => (pairsOf ps).map ((k, v) :: ·)
  | _ => none

/-- A map from key-value pairs, set in order: a later duplicate key replaces
the value and keeps the first one's place (`List.to_map`, reference 14). -/
def mapOfPairs (ps : List (Value × Value)) : List (Value × Value) :=
  ps.foldl (fun m kv => mapSet m kv.1 kv.2) []

/-- The entries of a map literal's arguments: key, value, key, value, ... -/
def litEntries : List Value → Option (List (Value × Value))
  | [] => some []
  | k :: x :: rest => (litEntries rest).map ((k, x) :: ·)
  | [_] => none

/-- Whether no two keys of a list of entries are equal. -/
def distinctKeys : List (Value × Value) → Bool
  | [] => true
  | (k, _) :: es => !(es.any fun e => Value.equiv k e.1) && distinctKeys es

def evalMapOp (op : MapOp) (vs : List Value) (w : World) : Outcome :=
  match op, vs with
  | .lit _, args =>
    match litEntries args with
    | some es => if distinctKeys es then .ok (.map es) w else .stopped .repeatedKey w
    | none => wrongArgs "a map literal"
  | .empty, [] => .ok (.map []) w
  | .size, [.map es] => okInt es.length w
  | .isEmpty, [.map es] => .ok (.bool es.isEmpty) w
  | .get, [.map es, k] => .ok (optOf (mapGet k es)) w
  | .has, [.map es, k] => .ok (.bool (mapGet k es).isSome) w
  | .set, [.map es, k, v] => .ok (.map (mapSet es k v)) w
  | .remove, [.map es, k] => .ok (.map (mapRemove es k)) w
  | .keys, [.map es] => .ok (.list (es.map Prod.fst)) w
  | .values, [.map es] => .ok (.list (es.map Prod.snd)) w
  | .entries, [.map es] => .ok (.list (es.map fun e => .tuple [e.1, e.2])) w
  | .ofList, [.list ps] =>
    match pairsOf ps with
    | some kvs => .ok (.map (mapOfPairs kvs)) w
    | none => wrongArgs "List.to_map"
  | _, _ => wrongArgs "Map primitive"

/-! ## Capabilities and the world -/

/-- A file error value. -/
def fileErrorV (ctor path : String) : Value := .con ctor [.text path]

/-- The parts of a path inside a folder: `.` parts are dropped, so `"."` is the
folder itself, and a `..` takes back the part before it. `none` for a path
that would leave the folder (a `..` with nothing before it to take back), and
for one that isn't relative and written with `/`, which has an empty part, as
in `""`, `/a`, `a//b` and `a/`. -/
def pathParts (path : String) : Option (List String) :=
  (textSplit path "/").foldl (fun acc part => acc.bind fun ps =>
    if part = "" then none
    else if part = "." then some ps
    else if part = ".." then (if ps.isEmpty then none else some ps.dropLast)
    else some (ps ++ [part])) (some [])

/-- A path of a handle whose folder is `root`, as a path from the folder the
program started in. `none` when the path would leave the handle's folder, or
when the handle reaches nothing (`root = none`); reading or writing such a
path is `Error(NotAllowed(path))` (reference 14). -/
def resolvePath : Option String → String → Option String
  | none, _ => none
  | some root, path =>
    (pathParts path).map fun parts =>
      "/".intercalate ((if root = "" then [] else textSplit root "/") ++ parts)

/-- The names directly inside a folder, sorted, without duplicates. -/
def folderNames (dir : String) (files : List (String × String)) : List String :=
  let pre := if dir = "" then "" else dir ++ "/"
  let names := files.filterMap fun e =>
    if textStartsWith e.1 pre then (textSplit (String.ofList (e.1.toList.drop pre.length)) "/").head?
    else none
  sortTexts (names.foldl (fun acc n => if acc.contains n then acc else acc ++ [n]) [])

/-- Sets a file's text: an existing file keeps its place. -/
def setFile (files : List (String × String)) (p text : String) : List (String × String) :=
  if files.any (·.1 = p) then files.map fun e => if e.1 = p then (p, text) else e
  else files ++ [(p, text)]

/-- The next state and draw of the random number generator (SplitMix64). -/
def nextRandom (s : Nat) : Nat × Nat :=
  let s' := (s + 0x9E3779B97F4A7C15) % 18446744073709551616
  let z := ((s' ^^^ (s' >>> 30)) * 0xBF58476D1CE4E5B9) % 18446744073709551616
  let z := ((z ^^^ (z >>> 27)) * 0x94D049BB133111EB) % 18446744073709551616
  (z ^^^ (z >>> 31), s')

/-- The root of a capability value of the expected kind. -/
def capRoot (expected : Cap) : Value → Option (Option String)
  | .cap c root => if c = expected then some root else none
  | _ => none

def filesKind (ro : Bool) : Cap := if ro then .readFiles else .files

def evalIoOp (op : IoOp) (vs : List Value) (w : World) : Outcome :=
  match op, vs with
  | .argsList, [h] =>
    match capRoot .args h with
    | some _ => .ok (texts w.args) w
    | none => wrongArgs "Args.list"
  | .envGet, [h, .text name] =>
    match capRoot .env h with
    | some _ => .ok (optText (lookup name w.env)) w
    | none => wrongArgs "Env.get"
  | .print, [h, .text s] =>
    match capRoot .console h with
    | some _ => .ok .unit { w with stdout := w.stdout ++ [s] }
    | none => wrongArgs "Console.print"
  | .printError, [h, .text s] =>
    match capRoot .console h with
    | some _ => .ok .unit { w with stderr := w.stderr ++ [s] }
    | none => wrongArgs "Console.error"
  | .readLine, [h] =>
    match capRoot .console h with
    | some _ =>
      match w.stdin with
      | [] => .ok noneV w
      | l :: rest => .ok (someV (.text l)) { w with stdin := rest }
    | none => wrongArgs "Console.read_line"
  | .printed, [h] =>
    match capRoot .console h with
    | some _ => .ok (texts w.stdout) w
    | none => wrongArgs "Console.printed"
  | .printedErrors, [h] =>
    match capRoot .console h with
    | some _ => .ok (texts w.stderr) w
    | none => wrongArgs "Console.printed_errors"
  | .readText ro, [h, .text path] =>
    match capRoot (filesKind ro) h with
    | some root =>
      match resolvePath root path with
      | none => .ok (errorV (fileErrorV "NotAllowed" path)) w
      | some p =>
        match lookup p w.files with
        | some text => .ok (okV (.text text)) w
        | none => .ok (errorV (fileErrorV "NotFound" path)) w
    | none => wrongArgs "Files.read_text"
  | .readBytes ro, [h, .text path] =>
    -- The world keeps each file as its text, so a file's bytes are its text's
    -- UTF-8 encoding.
    match capRoot (filesKind ro) h with
    | some root =>
      match resolvePath root path with
      | none => .ok (errorV (fileErrorV "NotAllowed" path)) w
      | some p =>
        match lookup p w.files with
        | some text => .ok (okV (.bytes (textBytes text))) w
        | none => .ok (errorV (fileErrorV "NotFound" path)) w
    | none => wrongArgs "Files.read_bytes"
  | .writeText, [h, .text path, .text text] =>
    match capRoot .files h with
    | some root =>
      match resolvePath root path with
      | none => .ok (errorV (fileErrorV "NotAllowed" path)) w
      | some p => .ok (okV .unit) { w with files := setFile w.files p text }
    | none => wrongArgs "Files.write_text"
  | .appendText, [h, .text path, .text text] =>
    match capRoot .files h with
    | some root =>
      match resolvePath root path with
      | none => .ok (errorV (fileErrorV "NotAllowed" path)) w
      | some p =>
        .ok (okV .unit) { w with files := setFile w.files p ((lookup p w.files).getD "" ++ text) }
    | none => wrongArgs "Files.append_text"
  | .remove, [h, .text path] =>
    match capRoot .files h with
    | some root =>
      match resolvePath root path with
      | none => .ok (errorV (fileErrorV "NotAllowed" path)) w
      | some p =>
        if w.files.any (·.1 = p) then .ok (okV .unit) { w with files := w.files.filter (·.1 ≠ p) }
        else .ok (errorV (fileErrorV "NotFound" path)) w
    | none => wrongArgs "Files.remove"
  | .list ro, [h, .text folder] =>
    match capRoot (filesKind ro) h with
    | some root =>
      match resolvePath root folder with
      | none => .ok (errorV (fileErrorV "NotAllowed" folder)) w
      | some dir =>
        let names := folderNames dir w.files
        if names.isEmpty && dir ≠ "" then .ok (errorV (fileErrorV "NotFound" folder)) w
        else .ok (okV (texts names)) w
    | none => wrongArgs "Files.list"
  | .exists ro, [h, .text path] =>
    match capRoot (filesKind ro) h with
    | some root =>
      match resolvePath root path with
      | none => .ok (.bool false) w
      | some p =>
        -- The top folder, the one the program started in or a fake stands
        -- for, always exists; any other folder exists when a file is inside it.
        .ok (.bool (p == "" || w.files.any (·.1 = p) || !(folderNames p w.files).isEmpty)) w
    | none => wrongArgs "Files.exists?"
  | .within ro, [h, .text folder] =>
    -- `within` a path that would leave the folder gives a handle that reaches
    -- nothing (reference 11.3, 14).
    match capRoot (filesKind ro) h with
    | some root => .ok (.cap (filesKind ro) (resolvePath root folder)) w
    | none => wrongArgs "Files.within"
  | .readOnly, [h] =>
    match capRoot .files h with
    | some root => .ok (.cap .readFiles root) w
    | none => wrongArgs "Files.read_only"
  | .now, [h] =>
    match capRoot .clock h with
    | some _ => okTime w.now w
    | none => wrongArgs "Clock.now"
  | .randomInt, [h, .int lo, .int hi] =>
    match capRoot .random h with
    | some _ =>
      if lo < hi then
        let (z, s') := nextRandom w.seed
        okInt (lo + (z : Int) % (hi - lo)) { w with seed := s' }
      else outside "Random.int needs low below high" w
    | none => wrongArgs "Random.int"
  | _, _ => wrongArgs "capability primitive"

/-! ## Dispatch -/

/-- Applies a primitive to its argument values in a world. -/
def Prim.eval : Prim → List Value → World → Outcome
  | .int op, vs, w => evalIntOp op vs w
  | .big op, vs, w => evalBigOp op vs w
  | .byte op, vs, w => evalByteOp op vs w
  | .dec op, vs, w => evalDecOp op vs w
  | .float op, vs, w => evalFloatOp op vs w
  | .not, [.bool b], w => .ok (.bool !b) w
  | .not, _, _ => wrongArgs "not"
  | .cmp b op, vs, w => evalCmp b op vs w
  | .char op, vs, w => evalCharOp op vs w
  | .text op, vs, w => evalTextOp op vs w
  | .bytes op, vs, w => evalBytesOp op vs w
  | .range op, vs, w => evalRangeOp op vs w
  | .time op, vs, w => evalTimeOp op vs w
  | .list _ op, vs, w => evalListOp op vs w
  | .map _ _ op, vs, w => evalMapOp op vs w
  | .eq _, [a, b], w => .ok (.bool (Value.equiv a b)) w
  | .eq _, _, _ => wrongArgs "=="
  | .io op, vs, w => evalIoOp op vs w

end MoCore
