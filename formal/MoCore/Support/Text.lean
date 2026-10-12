/-
  Mo Core support: text.

  The text functions of reference 14, written over lists of characters so that
  their meaning is plain. Text is Lean's `String`: valid Unicode, compared by
  character code.
-/
import MoCore.Value

namespace MoCore

/-- ASCII white space: space, tab, line feed, vertical tab, form feed and
carriage return. -/
def isAsciiWhite (c : Char) : Bool :=
  c = ' ' || c = '\t' || c = '\n' || c = '\r' || c.toNat = 11 || c.toNat = 12

def isAsciiLetter (c : Char) : Bool :=
  ('a'.toNat ≤ c.toNat && c.toNat ≤ 'z'.toNat) || ('A'.toNat ≤ c.toNat && c.toNat ≤ 'Z'.toNat)

def asciiLowerChar (c : Char) : Char :=
  if 'A'.toNat ≤ c.toNat && c.toNat ≤ 'Z'.toNat then Char.ofNat (c.toNat + 32) else c

def asciiUpperChar (c : Char) : Char :=
  if 'a'.toNat ≤ c.toNat && c.toNat ≤ 'z'.toNat then Char.ofNat (c.toNat - 32) else c

/-- Splits a list at every element satisfying `p`, keeping empty pieces. -/
def splitWhen {α : Type} (p : α → Bool) : List α → List (List α)
  | [] => [[]]
  | c :: cs =>
    match splitWhen p cs with
    | [] => [[c]]  -- unreachable: `splitWhen` never returns `[]`
    | piece :: pieces => if p c then [] :: piece :: pieces else (c :: piece) :: pieces

/-- Splits at every occurrence of a non-empty separator, keeping empty pieces.
`fuel` bounds the walk; the length of the text is enough. -/
def splitOnSep (sep : List Char) : Nat → List Char → List Char → List (List Char)
  | 0, cur, _ => [cur.reverse]
  | _ + 1, cur, [] => [cur.reverse]
  | fuel + 1, cur, c :: rest =>
    if !sep.isEmpty && sep.isPrefixOf (c :: rest) then
      cur.reverse :: splitOnSep sep fuel [] ((c :: rest).drop sep.length)
    else splitOnSep sep fuel (c :: cur) rest

/-- `text.split(separator)`: the separator isn't empty, and empty pieces are kept. -/
def textSplit (s sep : String) : List String :=
  (splitOnSep sep.toList (s.length + 1) [] s.toList).map String.ofList

/-- `text.lines`: split where `\n` or `\r\n` ends a line, without the line
endings. `""` has none, a final line break doesn't start another line, and a
`\r` alone stays in its line (reference 14). -/
def textLines (s : String) : List String :=
  let cs := s.toList
  if cs.isEmpty then []
  else
    let pieces := splitWhen (· = '\n') cs
    -- Every piece but the last ended at a `\n`, so a `\r` at its end was part
    -- of a `\r\n`.
    let ended := pieces.dropLast.map fun p => if p.getLast? = some '\r' then p.dropLast else p
    -- The last piece ran to the end of the text: a line, unless the text ended
    -- with a line break. A `\r` at its end is alone, so it stays.
    let last := if cs.getLast? = some '\n' then [] else pieces.getLast?.toList
    (ended ++ last).map String.ofList

/-- `text.words`: split at ASCII white space, which alone gives none. -/
def textWords (s : String) : List String :=
  ((splitWhen isAsciiWhite s.toList).filter (!·.isEmpty)).map String.ofList

def textTrimStart (s : String) : String := String.ofList (s.toList.dropWhile isAsciiWhite)
def textTrimEnd (s : String) : String :=
  String.ofList (s.toList.reverse.dropWhile isAsciiWhite).reverse
def textTrim (s : String) : String := textTrimEnd (textTrimStart s)

def textStartsWith (s pre : String) : Bool := pre.toList.isPrefixOf s.toList
def textEndsWith (s suf : String) : Bool := suf.toList.isSuffixOf s.toList

/-- Whether `part` occurs in `cs`. -/
def containsChars (part : List Char) : List Char → Bool
  | [] => part.isEmpty
  | c :: cs => part.isPrefixOf (c :: cs) || containsChars part cs

def textContains (s part : String) : Bool := containsChars part.toList s.toList

def textDropPrefix (s pre : String) : Option String :=
  if textStartsWith s pre then some (String.ofList (s.toList.drop pre.length)) else none

def textDropSuffix (s suf : String) : Option String :=
  if textEndsWith s suf then some (String.ofList (s.toList.take (s.length - suf.length))) else none

/-- `text.replace(old, new)` for a non-empty `old`: every occurrence, left to right. -/
def textReplace (s old new : String) : String := String.intercalate new (textSplit s old)

def textRepeat (s : String) (n : Nat) : String := String.join (List.replicate n s)

/-- Padding counts characters, and leaves text already as wide unchanged. -/
def textPadLeft (s : String) (width : Int) : String :=
  String.ofList (List.replicate (width.toNat - s.length) ' ') ++ s

def textPadRight (s : String) (width : Int) : String :=
  s ++ String.ofList (List.replicate (width.toNat - s.length) ' ')

def textLower (s : String) : String := String.ofList (s.toList.map asciiLowerChar)
def textUpper (s : String) : String := String.ofList (s.toList.map asciiUpperChar)

/-- Texts sort by character code. -/
def charsLt : List Char → List Char → Bool
  | [], [] => false
  | [], _ :: _ => true
  | _ :: _, [] => false
  | a :: as, b :: bs =>
    if a.toNat < b.toNat then true else if b.toNat < a.toNat then false else charsLt as bs

def textLt (a b : String) : Bool := charsLt a.toList b.toList

/-- Sorts texts by character code (insertion sort; stable). -/
def sortTexts (xs : List String) : List String :=
  xs.foldl (fun sorted x => insert x sorted) []
where
  insert (x : String) : List String → List String
    | [] => [x]
    | y :: ys => if textLt x y then x :: y :: ys else y :: insert x ys

/-- A code point that is a Unicode scalar value, as a `Char`. -/
def charOfCode (n : Int) : Option Char :=
  if 0 ≤ n && n ≤ 0x10FFFF && !(0xD800 ≤ n && n ≤ 0xDFFF) then some (Char.ofNat n.toNat) else none

end MoCore
