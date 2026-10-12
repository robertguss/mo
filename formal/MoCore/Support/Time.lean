/-
  Mo Core support: times and durations.

  A `Time` counts microseconds since 1970-01-01T00:00:00Z, in the proleptic
  Gregorian calendar; a `Duration` counts microseconds. This file converts
  times to and from RFC 3339 text (reference 3.9 and 14).
-/
import MoCore.Support.Num

namespace MoCore

/-- Days since 1970-01-01 of a calendar date (Hinnant's `days_from_civil`). -/
def daysFromCivil (y m d : Int) : Int :=
  let y := if m ≤ 2 then y - 1 else y
  let era := y.fdiv 400
  let yoe := y - era * 400
  let mp := if m > 2 then m - 3 else m + 9
  let doy := (153 * mp + 2).fdiv 5 + d - 1
  let doe := yoe * 365 + yoe.fdiv 4 - yoe.fdiv 100 + doy
  era * 146097 + doe - 719468

/-- The calendar date of a day count (Hinnant's `civil_from_days`). -/
def civilFromDays (z : Int) : Int × Int × Int :=
  let z := z + 719468
  let era := z.fdiv 146097
  let doe := z - era * 146097
  let yoe := (doe - doe.fdiv 1460 + doe.fdiv 36524 - doe.fdiv 146096).fdiv 365
  let y := yoe + era * 400
  let doy := doe - (365 * yoe + yoe.fdiv 4 - yoe.fdiv 100)
  let mp := (5 * doy + 2).fdiv 153
  let d := doy - (153 * mp + 2).fdiv 5 + 1
  let m := if mp < 10 then mp + 3 else mp - 9
  (if m ≤ 2 then y + 1 else y, m, d)

def isLeapYear (y : Int) : Bool := (y % 4 = 0 && y % 100 ≠ 0) || y % 400 = 0

def daysInMonth (y m : Int) : Int :=
  if m = 2 then (if isLeapYear y then 29 else 28)
  else if m = 4 || m = 6 || m = 9 || m = 11 then 30 else 31

def microsPerSecond : Int := 1000000
def microsPerDay : Int := 86400000000

/-- Two digits, with a leading zero. -/
def twoDigits (n : Int) : String := padZeros (toString n.toNat) 2

/-- A time as RFC 3339 text in UTC, such as `2026-10-12T09:00:00Z`; fractions
of a second are written only when there are some, without trailing zeros. -/
def timeToText (t : Int) : String :=
  let days := t.fdiv microsPerDay
  let inDay := t - days * microsPerDay
  let (y, m, d) := civilFromDays days
  let secs := inDay.fdiv microsPerSecond
  let frac := inDay - secs * microsPerSecond
  let hh := secs.fdiv 3600
  let mm := (secs - hh * 3600).fdiv 60
  let ss := secs - hh * 3600 - mm * 60
  let fracText :=
    if frac = 0 then ""
    else "." ++ String.ofList (dropTrailingZeros (padZeros (toString frac.toNat) 6).toList)
  padZeros (toString y.toNat) 4 ++ "-" ++ twoDigits m ++ "-" ++ twoDigits d ++ "T" ++
    twoDigits hh ++ ":" ++ twoDigits mm ++ ":" ++ twoDigits ss ++ fracText ++ "Z"

/-- A duration's text form: in seconds, as `1.5s` (reference 9.2). -/
def durationToText (d : Int) : String := decToText (d * 1000000000000) ++ "s"

/-- Exactly `n` ASCII digits at the front of a list, and the rest. -/
def takeDigits (n : Nat) (cs : List Char) : Option (Nat × List Char) :=
  let ds := cs.take n
  if ds.length = n && ds.all isDigitChar then some (digitsValue ds, cs.drop n) else none

/-- One expected character at the front of a list. -/
def expectChar (c : Char) : List Char → Option (List Char)
  | d :: rest => if c = d then some rest else none
  | [] => none

/-- `Time.parse`: RFC 3339 text, `YYYY-MM-DDTHH:MM:SS`, an optional fraction of
one to six digits, then `Z` or an offset `+HH:MM` or `-HH:MM`. `T` and `Z` may
be lowercase. `None` for anything else, or a time outside the years 1 to 9999. -/
def parseTime (s : String) : Option Int := do
  let cs := s.toList
  let (y, cs) ← takeDigits 4 cs
  let cs ← expectChar '-' cs
  let (mo, cs) ← takeDigits 2 cs
  let cs ← expectChar '-' cs
  let (d, cs) ← takeDigits 2 cs
  let cs ← (match cs with
    | 'T' :: rest | 't' :: rest => some rest
    | _ => none)
  let (hh, cs) ← takeDigits 2 cs
  let cs ← expectChar ':' cs
  let (mi, cs) ← takeDigits 2 cs
  let cs ← expectChar ':' cs
  let (ss, cs) ← takeDigits 2 cs
  let (frac, cs) ← (match cs with
    | '.' :: rest =>
      let ds := rest.takeWhile isDigitChar
      if ds.isEmpty || ds.length > 6 then none
      else some ((digitsValue ds : Int) * 10 ^ (6 - ds.length), rest.drop ds.length)
    | _ => some (0, cs))
  let offset ← (match cs with
    | ['Z'] | ['z'] => some (0 : Int)
    | sign :: rest =>
      if sign = '+' || sign = '-' then
        match takeDigits 2 rest with
        | some (oh, ':' :: rest') =>
          match takeDigits 2 rest' with
          | some (om, []) =>
            if oh ≤ 23 && om ≤ 59 then
              some ((if sign = '-' then -1 else 1) * ((oh : Int) * 3600 + (om : Int) * 60))
            else none
          | _ => none
        | _ => none
      else none
    | [] => none)
  let (y, mo, d, hh, mi, ss) : Int × Int × Int × Int × Int × Int := (y, mo, d, hh, mi, ss)
  if 1 ≤ mo && mo ≤ 12 && 1 ≤ d && d ≤ daysInMonth y mo && hh ≤ 23 && mi ≤ 59 && ss ≤ 59 then
    let t := ((daysFromCivil y mo d * 86400 + hh * 3600 + mi * 60 + ss) - offset) * microsPerSecond + frac
    if InTime t then some t else none
  else none

/-- A time for writing Core programs, such as `timeLit "2026-10-12T09:00:00Z"`.
Ill-formed text gives the Unix epoch. -/
def timeLit (s : String) : Int := (parseTime s).getD 0

end MoCore
