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

end MoCore
