/-
  Mo Core support: bytes.

  A `Bytes` value is a list of whole numbers from 0 to 255. This file writes
  text as UTF-8 (`Text.bytes`) and reads UTF-8 back (`Bytes.decode_utf8`),
  over lists so that their meaning is plain, and orders bytes byte by byte
  (reference 9.2).
-/
import MoCore.Value

namespace MoCore

/-! ## UTF-8 -/

/-- The UTF-8 encoding of a character (RFC 3629): one byte below U+0080, two
below U+0800, three below U+10000, and four from there to U+10FFFF. -/
def utf8EncodeChar (c : Char) : List Int :=
  let n : Int := c.toNat
  if n < 0x80 then [n]
  else if n < 0x800 then [0xC0 + n / 0x40, 0x80 + n % 0x40]
  else if n < 0x10000 then [0xE0 + n / 0x1000, 0x80 + n / 0x40 % 0x40, 0x80 + n % 0x40]
  else [0xF0 + n / 0x40000, 0x80 + n / 0x1000 % 0x40, 0x80 + n / 0x40 % 0x40, 0x80 + n % 0x40]

/-- `text.bytes`: the text's UTF-8 encoding, which always exists (reference
3.4). It has as many bytes as `byte_count` counts. -/
def textBytes (s : String) : List Int := s.toList.flatMap utf8EncodeChar

/-- A continuation byte: `0x80` to `0xBF`. -/
def isContByte (b : Int) : Bool := 0x80 ≤ b && b ≤ 0xBF

/-- The well-formed UTF-8 sequences that start with the byte `b`, other than
the one-byte ones, row by row as table 3-7 of the Unicode Standard lists them:
how many continuation bytes follow, the range the first of them must be in,
and the value `b` contributes. The narrower ranges after `E0`, `ED`, `F0` and
`F4` rule out overlong forms, the surrogates (U+D800 to U+DFFF) and anything
above U+10FFFF. `none` for a byte that can't start a sequence: a continuation
byte, `C0`, `C1`, or `F5` and above. -/
def utf8Lead (b : Int) : Option (Nat × Int × Int × Int) :=
  if 0xC2 ≤ b ∧ b ≤ 0xDF then some (1, 0x80, 0xBF, b - 0xC0)
  else if b = 0xE0 then some (2, 0xA0, 0xBF, 0x0)
  else if 0xE1 ≤ b ∧ b ≤ 0xEC then some (2, 0x80, 0xBF, b - 0xE0)
  else if b = 0xED then some (2, 0x80, 0x9F, 0xD)
  else if 0xEE ≤ b ∧ b ≤ 0xEF then some (2, 0x80, 0xBF, b - 0xE0)
  else if b = 0xF0 then some (3, 0x90, 0xBF, 0x0)
  else if 0xF1 ≤ b ∧ b ≤ 0xF3 then some (3, 0x80, 0xBF, b - 0xF0)
  else if b = 0xF4 then some (3, 0x80, 0x8F, 0x4)
  else none

/-- `bytes.decode_utf8`: the characters the bytes encode, or `none` when they
aren't well-formed UTF-8 (reference 3.4): a byte that can't start a sequence,
a continuation byte out of its range, or a sequence cut short by the end. A
byte order mark, `EF BB BF`, is the character U+FEFF like any other: nothing
is dropped. -/
def utf8Decode (bs : List Int) : Option (List Char) :=
  match bs with
  | [] => some []
  | b :: rest =>
    if 0 ≤ b ∧ b ≤ 0x7F then (utf8Decode rest).map (Char.ofNat b.toNat :: ·)
    else
      match utf8Lead b with
      | none => none
      | some (k, lo, hi, lead) =>
        let cont := rest.take k
        if cont.length = k ∧ cont.head?.all (fun c => lo ≤ c && c ≤ hi) ∧ cont.all isContByte then
          let n := cont.foldl (fun n c => n * 0x40 + (c - 0x80)) lead
          (utf8Decode (rest.drop k)).map (Char.ofNat n.toNat :: ·)
        else none
termination_by bs.length
decreasing_by all_goals simp only [List.length_cons, List.length_drop]; omega

/-! ## Order -/

/-- Bytes sort byte by byte (reference 9.2): at the first place two sequences
differ, the smaller byte comes first, and a sequence comes before any longer
one that begins with it, as a list does. -/
def bytesLt : List Int → List Int → Bool
  | [], [] => false
  | [], _ :: _ => true
  | _ :: _, [] => false
  | a :: as, b :: bs => if a < b then true else if b < a then false else bytesLt as bs

/-! ## Hex and Base64url (RFC 4648) -/

def hexDigit (n : Int) : Char :=
  if n < 10 then Char.ofNat ('0'.toNat + n.toNat)
  else Char.ofNat ('a'.toNat + (n - 10).toNat)

def bytesToHex (bs : List Int) : String :=
  String.ofList (bs.flatMap fun b => [hexDigit (b / 16), hexDigit (b % 16)])

def hexVal (c : Char) : Option Int :=
  let n : Int := c.toNat
  if 48 ≤ n ∧ n ≤ 57 then some (n - 48)
  else if 97 ≤ n ∧ n ≤ 102 then some (n - 87)
  else if 65 ≤ n ∧ n ≤ 70 then some (n - 55)
  else none

def hexDecodeChars : List Char → Option (List Int)
  | [] => some []
  | c1 :: c2 :: rest =>
    match hexVal c1, hexVal c2, hexDecodeChars rest with
    | some hi, some lo, some tail => some ((hi * 16 + lo) :: tail)
    | _, _, _ => none
  | [_] => none

def bytesFromHex (s : String) : Option (List Int) := hexDecodeChars s.toList

def b64urlChar (n : Int) : Char :=
  if n < 26 then Char.ofNat ('A'.toNat + n.toNat)
  else if n < 52 then Char.ofNat ('a'.toNat + (n - 26).toNat)
  else if n < 62 then Char.ofNat ('0'.toNat + (n - 52).toNat)
  else if n = 62 then '-'
  else '_'

def b64urlEncodeBytes : List Int → List Char
  | [] => []
  | [b0] => [b64urlChar (b0 / 4), b64urlChar ((b0 % 4) * 16)]
  | [b0, b1] =>
    [b64urlChar (b0 / 4), b64urlChar ((b0 % 4) * 16 + b1 / 16), b64urlChar ((b1 % 16) * 4)]
  | b0 :: b1 :: b2 :: rest =>
    b64urlChar (b0 / 4) :: b64urlChar ((b0 % 4) * 16 + b1 / 16) ::
    b64urlChar ((b1 % 16) * 4 + b2 / 64) :: b64urlChar (b2 % 64) ::
    b64urlEncodeBytes rest

def bytesToBase64url (bs : List Int) : String :=
  String.ofList (b64urlEncodeBytes bs)

def b64urlVal (c : Char) : Option Int :=
  let n : Int := c.toNat
  if 65 ≤ n ∧ n ≤ 90 then some (n - 65)
  else if 97 ≤ n ∧ n ≤ 122 then some (n - 71)
  else if 48 ≤ n ∧ n ≤ 57 then some (n + 4)
  else if n = 45 then some 62
  else if n = 95 then some 63
  else none

def b64urlDecodeChars : List Char → Option (List Int)
  | [] => some []
  | [_] => none
  | [c0, c1] =>
    match b64urlVal c0, b64urlVal c1 with
    | some v0, some v1 =>
      if v1 % 16 = 0 then some [v0 * 4 + v1 / 16] else none
    | _, _ => none
  | [c0, c1, c2] =>
    match b64urlVal c0, b64urlVal c1, b64urlVal c2 with
    | some v0, some v1, some v2 =>
      if v2 % 4 = 0 then some [v0 * 4 + v1 / 16, (v1 % 16) * 16 + v2 / 4] else none
    | _, _, _ => none
  | c0 :: c1 :: c2 :: c3 :: rest =>
    match b64urlVal c0, b64urlVal c1, b64urlVal c2, b64urlVal c3, b64urlDecodeChars rest with
    | some v0, some v1, some v2, some v3, some tail =>
      some ((v0 * 4 + v1 / 16) :: ((v1 % 16) * 16 + v2 / 4) :: ((v2 % 4) * 64 + v3) :: tail)
    | _, _, _, _, _ => none

def bytesFromBase64url (s : String) : Option (List Int) := b64urlDecodeChars s.toList

end MoCore
