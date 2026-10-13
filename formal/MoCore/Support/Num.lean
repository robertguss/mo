/-
  Mo Core support: whole numbers and decimals.

  Exact arithmetic on Lean's `Int`, with the rounding rules of reference 3.2
  and 3.3, and the text forms and parsers of reference 14. These are plain
  functions; the primitives in `MoCore/Prim.lean` add the range checks.
-/
import MoCore.Value

namespace MoCore

/-! ## Wrapping and saturating 64-bit arithmetic -/

/-- Wraps a whole number into the 64-bit range, as two's complement does. -/
def wrap64 (n : Int) : Int :=
  let m := n % 18446744073709551616
  if m ≥ 9223372036854775808 then m - 18446744073709551616 else m

/-- Caps a whole number at the ends of the 64-bit range. -/
def saturate64 (n : Int) : Int :=
  if n > int64Max then int64Max else if n < int64Min then int64Min else n

/-! ## Rounding -/

/-- `num / den` rounded to the nearest whole number, an exact tie to the even
one (reference 3.3). `den` must not be 0. -/
def roundHalfEven (num den : Int) : Int :=
  let n := if den < 0 then -num else num
  let d := if den < 0 then -den else den
  let q := n.fdiv d
  let r := n - q * d
  if 2 * r < d then q
  else if 2 * r > d then q + 1
  else if q % 2 = 0 then q else q + 1

/-- `num / den` rounded to the nearest whole number, a tie away from zero, as
people round by hand (`Decimal.round`). `den` must be positive. -/
def roundHalfAway (num den : Int) : Int :=
  let q : Int := (2 * num.natAbs + den.toNat) / (2 * den.toNat)
  if num < 0 then -q else q

/-! ## Decimal arithmetic on counts of `10^-18` -/

/-- The product of two decimals, rounded to 18 places, ties to even. -/
def decMul (a b : Int) : Int := roundHalfEven (a * b) decScale

/-- The quotient of two decimals, rounded to 18 places, ties to even. `b ≠ 0`. -/
def decDiv (a b : Int) : Int := roundHalfEven (a * decScale) b

/-- A decimal rounded to `places` places (at most 18), ties away from zero. -/
def decRound (a : Int) (places : Nat) : Int :=
  let q : Int := 10 ^ (18 - places)
  roundHalfAway a q * q

/-- The largest whole decimal not above `a`. -/
def decFloor (a : Int) : Int := a.fdiv decScale * decScale

/-- The smallest whole decimal not below `a`. -/
def decCeil (a : Int) : Int := -((-a).fdiv decScale * decScale)

/-! ## Text forms -/

/-- Leading zeros up to a width. -/
def padZeros (s : String) (width : Nat) : String :=
  String.ofList (List.replicate (width - s.length) '0') ++ s

/-- A list of characters without its trailing zeros. -/
def dropTrailingZeros (cs : List Char) : List Char :=
  (cs.reverse.dropWhile (· = '0')).reverse

/-- A decimal's text form: its shortest exact spelling, such as `15.5`, `3`
or `0.001` (reference 3.3). -/
def decToText (n : Int) : String :=
  let sign := if n < 0 then "-" else ""
  let a := n.natAbs
  let ip := a / 1000000000000000000
  let fp := a % 1000000000000000000
  if fp = 0 then sign ++ toString ip
  else sign ++ toString ip ++ "." ++ String.ofList (dropTrailingZeros (padZeros (toString fp) 18).toList)

/-- `to_fixed(places)`: exactly `places` places (at most 18), rounded as
`round` does. -/
def decToFixed (n : Int) (places : Nat) : String :=
  let r := decRound n places
  let sign := if r < 0 then "-" else ""
  let a := r.natAbs
  let ip := a / 1000000000000000000
  let fp := a % 1000000000000000000
  if places = 0 then sign ++ toString ip
  else sign ++ toString ip ++ "." ++ String.ofList ((padZeros (toString fp) 18).toList.take places)

/-! ## Exact binary64 conversion -/

/-- Scale an exact positive ratio by an integral power of a base. -/
def scaleRatio (n d base : Nat) (e : Int) : Nat × Nat :=
  if e ≥ 0 then (n * base ^ e.toNat, d) else (n, d * base ^ (-e).toNat)

/-- Round an exact ratio once to binary64, ties to even. `d` is positive.
The exponent comes from integer bit lengths and one exact comparison. Subnormals
round in units of 2^-1074; a significand carry also handles the overflow tie.
No floating-point arithmetic or `Float.ofScientific` participates. -/
def ratioToFloatBits (negative : Bool) (n d : Nat) : UInt64 :=
  let sign : Nat := if negative then 2 ^ 63 else 0
  let infinity : Nat := 0x7ff0000000000000
  if n = 0 then UInt64.ofNat sign else
    let k : Int := (n.log2 : Int) - (d.log2 : Int)
    let (a, b) := scaleRatio n d 2 (-k)
    let k := if a < b then k - 1 else k
    if k > 1023 then UInt64.ofNat (sign + infinity)
    else if k < -1075 then UInt64.ofNat sign
    else
      let e := max k (-1022)
      let (a, b) := scaleRatio n d 2 (52 - e)
      let q := (roundHalfEven a b).toNat
      UInt64.ofNat (sign + min infinity ((e + 1022).toNat * 2 ^ 52 + q))

/-- A decimal as the nearest `Float`, with only the final rounding. -/
def decToFloat (n : Int) : Float :=
  Float.ofBits (ratioToFloatBits (n < 0) n.natAbs decScale.toNat)

/-- The exact positive ratio of finite binary64 magnitude bits. -/
def floatRatio (bits : UInt64) : Nat × Nat :=
  let b := bits.toNat % 2 ^ 63
  let e : Nat := b / 2 ^ 52
  let f := b % 2 ^ 52
  if e = 0 then (f, 2 ^ 1074)
  else scaleRatio (2 ^ 52 + f) 1 2 ((e : Int) - 1075)

/-- The nearest shortest round-tripping decimal coefficient and power of ten
for a finite magnitude. At most 17 significant digits suffice for binary64.
Candidate round trips use the exact converter above, including endpoint ties;
adjacent candidates cover the asymmetric interval at a power of two. -/
def shortestFloatDecimal (bits : UInt64) : Nat × Int :=
  let magnitude := UInt64.ofNat (bits.toNat % 2 ^ 63)
  let (n, d) := floatRatio magnitude
  if n = 0 then (0, 0) else
    let k : Int := ((toString n).length : Int) - ((toString d).length : Int)
    let (a, b) := scaleRatio n d 10 (-k)
    let k := if a < b then k - 1 else k
    let candidate (digits : Nat) : Nat × Int :=
      let e := k - (digits : Int) + 1
      let (a, b) := scaleRatio n d 10 (-e)
      ((roundHalfEven a b).toNat, e)
    let found := (List.range 17).findSome? fun i =>
      let (q, e) := candidate (i + 1)
      [q, q - 1, q + 1].findSome? fun c =>
        let (a, b) := scaleRatio c 1 10 e
        if ratioToFloatBits false a b == magnitude then some (c, e) else none
    found.getD (candidate 17)

/-- Render shortest round-trip digits in fixed notation for decimal exponents
from -6 through 20, scientific otherwise (D236). Both zeros stay in plain
digits; exponents have no plus sign or leading zeros. -/
def shortestFloatText (bits : UInt64) : String :=
  let sign := if bits.toNat ≥ 2 ^ 63 then "-" else ""
  let (c, e) := shortestFloatDecimal bits
  if c = 0 then sign ++ "0" else
    let full := (toString c).toList
    let ds := dropTrailingZeros full
    let e := e + (full.length - ds.length : Nat)
    let point := (ds.length : Int) + e
    let zeros (n : Nat) := String.ofList (List.replicate n '0')
    let fixed :=
      if point ≤ 0 then "0." ++ zeros (-point).toNat ++ String.ofList ds
      else if point ≥ ds.length then String.ofList ds ++ zeros (point.toNat - ds.length)
      else String.ofList (ds.take point.toNat) ++ "." ++ String.ofList (ds.drop point.toNat)
    let mantissa := String.ofList (ds.take 1) ++
      (if ds.length = 1 then "" else "." ++ String.ofList (ds.drop 1))
    let scientific := mantissa ++ "e" ++ toString (point - 1)
    sign ++ (if -5 ≤ point && point ≤ 21 then fixed else scientific)

/-- `Float.to_decimal` uses the shortest decimal, not the exact binary ratio,
then rounds once to 18 places, ties to even. Range checking is in the primitive. -/
def floatToDecimal (bits : UInt64) : Option Int :=
  if bits.toNat / 2 ^ 52 % 2048 = 2047 then none else
    let (c, e) := shortestFloatDecimal bits
    let (n, d) := scaleRatio c 1 10 (e + 18)
    let count := roundHalfEven n d
    some (if bits.toNat ≥ 2 ^ 63 then -count else count)

/-- Truncating Float remainder (C `fmod`), calculated on exact binary ratios.
NaN, an infinite dividend or a zero divisor gives NaN; a finite dividend with
an infinite divisor is unchanged. A zero remainder keeps the dividend's sign. -/
def floatRem (a b : UInt64) : UInt64 :=
  let am := a.toNat % 2 ^ 63
  let bm := b.toNat % 2 ^ 63
  if am ≥ 0x7ff0000000000000 || bm > 0x7ff0000000000000 || bm = 0 then
    0x7ff8000000000000
  else if bm = 0x7ff0000000000000 then a
  else
    let (an, ad) := floatRatio a
    let (bn, bd) := floatRatio b
    ratioToFloatBits (a.toNat ≥ 2 ^ 63) ((an * bd) % (bn * ad)) (ad * bd)

/-! ## Parsing -/

/-- An ASCII digit. -/
def isDigitChar (c : Char) : Bool := '0'.toNat ≤ c.toNat && c.toNat ≤ '9'.toNat

/-- The value of a list of ASCII digits. -/
def digitsValue (ds : List Char) : Nat :=
  ds.foldl (fun acc c => acc * 10 + (c.toNat - '0'.toNat)) 0

/-- An optional `-` sign. -/
def splitSign : List Char → Bool × List Char
  | '-' :: rest => (true, rest)
  | cs => (false, cs)

/-- `Text.to_int`: an optional `-` and ASCII digits, in the `Int` range, and
nothing else. -/
def parseIntText (s : String) : Option Int :=
  let (neg, ds) := splitSign s.toList
  if ds.isEmpty || !ds.all isDigitChar then none
  else
    let v : Int := if neg then -(digitsValue ds : Int) else digitsValue ds
    if InInt64 v then some v else none

/-- `Text.to_decimal`: an optional `-`, ASCII digits, and an optional `.`
followed by one to 18 digits, in the `Decimal` range. The count of `10^-18`. -/
def parseDecimalText (s : String) : Option Int :=
  let (neg, rest) := splitSign s.toList
  let ip := rest.takeWhile isDigitChar
  let after := rest.dropWhile isDigitChar
  let finish (v : Int) : Option Int :=
    let v := if neg then -v else v
    if InDecimal v then some v else none
  if ip.isEmpty then none
  else match after with
    | [] => finish ((digitsValue ip : Int) * decScale)
    | '.' :: fs =>
      if fs.isEmpty || fs.length > 18 || !fs.all isDigitChar then none
      else finish ((digitsValue ip : Int) * decScale + (digitsValue fs : Int) * 10 ^ (18 - fs.length))
    | _ => none

/-- `Text.to_float`, with reference 14's exact syntax. Large exponents are
classified before building powers of ten; underflow is signed zero and finite
overflow is `None`. Explicit `nan`/`inf`/`-inf` remain accepted. -/
def parseFloatText (s : String) : Option UInt64 := do
  if s = "nan" then return 0x7ff8000000000000
  if s = "inf" then return 0x7ff0000000000000
  if s = "-inf" then return 0xfff0000000000000
  let (negative, rest) := splitSign s.toList
  let ip := rest.takeWhile isDigitChar
  if ip.isEmpty then none else do
    let after := rest.dropWhile isDigitChar
    let (fp, after) ← match after with
      | '.' :: cs =>
        let fp := cs.takeWhile isDigitChar
        if fp.isEmpty then none else some (fp, cs.dropWhile isDigitChar)
      | cs => some ([], cs)
    let exponent ← match after with
      | [] => some (0 : Int)
      | 'e' :: cs | 'E' :: cs =>
        let (neg, ds) := match cs with
          | '+' :: ds => (false, ds)
          | cs => splitSign cs
        if ds.isEmpty || !ds.all isDigitChar then none
        else some (if neg then -(digitsValue ds : Int) else digitsValue ds)
      | _ => none
    let ds := (ip ++ fp).dropWhile (· = '0')
    let e := exponent - (fp.length : Int)
    let order := (ds.length : Int) + e
    if ds.isEmpty || order < -323 then
      return ratioToFloatBits negative 0 1
    if order > 309 then none else do
      let (n, d) := scaleRatio (digitsValue ds) 1 10 e
      let bits := ratioToFloatBits negative n d
      if bits.toNat % 2 ^ 63 ≥ 0x7ff0000000000000 then none else some bits

/-- A decimal literal for writing Core programs, such as `decLit "12.50"`.
Ill-formed text gives 0. -/
def decLit (s : String) : Int := (parseDecimalText s).getD 0

/-- A float's text form. A float that is not a number is `nan`, and the
infinities are `inf` and `-inf` (reference 9.2). Finite values use the same
shortest decimal as `to_decimal`, with exact round-trip checks. -/
def floatToText (x : Float) : String :=
  if x.isNaN then "nan" else if x.isInf then (if x < 0 then "-inf" else "inf")
  else shortestFloatText x.toBits

end MoCore
