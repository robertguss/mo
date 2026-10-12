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

/-- A decimal as the nearest `Float`. -/
def decToFloat (n : Int) : Float := Float.ofInt n / 1.0e18

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

/-- A decimal literal for writing Core programs, such as `decLit "12.50"`.
Ill-formed text gives 0. -/
def decLit (s : String) : Int := (parseDecimalText s).getD 0

end MoCore
