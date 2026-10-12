/-
  Mo Core: checks of the built-in types `Byte`, `Bytes` and `Range`.

  The twelve samples use these types little or not at all, so this file checks
  their primitives the way the samples check their functions: each check is a
  Core expression of type `Bool`, under the Mo it lowers from, which `#guard`
  checks is well typed and evaluates to `true` (`passes`).
-/
import MoCore.Testing

namespace MoCore.Builtins

open MoCore DSL Lib

/-- The library functions the checks call. -/
def program : Program := { data := [], funs := sortBy tByte tByte ++ sortBy tBytes tBytes }

/-- `x.to_byte`. -/
def toByte (x : Expr) : Expr := iop .toByte [x]

/-! ## Byte

A `Byte` holds 0 to 255 and has no arithmetic (reference 3.2); it has
`Equal`, `Hash`, `Order` and `Text` (reference 9.2). A number literal where a
`Byte` is expected is a byte literal, which must be from 0 to 255. -/

-- 255.to_byte == Some(255), 0.to_byte == Some(0)
#guard passes program (eq (tOpt tByte) (toByte (int 255)) (some_ tByte (byteE 255)))
#guard passes program (eq (tOpt tByte) (toByte (int 0)) (some_ tByte (byteE 0)))
-- 256.to_byte == None, (-1).to_byte == None
#guard passes program (eq (tOpt tByte) (toByte (int 256)) (none_ tByte))
#guard passes program (eq (tOpt tByte) (toByte (iop .neg [int 1])) (none_ tByte))

-- With b = 200.to_byte or stop "": b.to_int == 200, b.to_text == "200", "b = #{b}" == "b = 200"
#guard passes program
  (lets [("b", byteE 200)] <|
    and_ (ieq (prim (.byte .toInt) [v "b"]) (int 200)) <|
      and_ (teq (prim (.byte .toText) [v "b"]) (str "200"))
        (teq (cat [str "b = ", prim (.byte .toText) [v "b"]]) (str "b = 200")))

-- 7 < 200, 200 <= 200, 255 > 0 and not 7 >= 200, as bytes
#guard passes program
  (and_ (cmp .byte .lt (byteE 7) (byteE 200)) <|
    and_ (cmp .byte .le (byteE 200) (byteE 200)) <|
      and_ (cmp .byte .gt (byteE 255) (byteE 0)) (not_ (cmp .byte .ge (byteE 7) (byteE 200))))

-- 7 == 7 and 7 != 8, as bytes
#guard passes program (and_ (eq tByte (byteE 7) (byteE 7)) (ne tByte (byteE 7) (byteE 8)))

-- [200, 7, 255, 0].sort_by(fn(b) b end) == [0, 7, 200, 255]
#guard passes program
  (eq (tList tByte)
    (sortByL tByte tByte (listOf tByte [byteE 200, byteE 7, byteE 255, byteE 0]) (lam [("b", tByte)] (v "b")))
    (listOf tByte [byteE 0, byteE 7, byteE 200, byteE 255]))

-- Bytes are map keys: {7: "seven", 8: "eight"}.get(8) == Some("eight")
#guard passes program
  (eq (tOpt tText)
    (mop tByte tText .get [mapOf tByte tText [(byteE 7, str "seven"), (byteE 8, str "eight")], byteE 8])
    (some_ tText (str "eight")))

-- A byte literal is from 0 to 255: the checker refuses 256 and -1.
#guard typedAs? program [] (byteE 255) tByte
#guard !typedAs? program [] (byteE 256) tByte
#guard !typedAs? program [] (byteE (-1)) tByte

/-! ## Bytes

`Bytes` is binary data: `text.bytes` always succeeds, and `decode_utf8` gives
`None` for bytes that aren't valid UTF-8 (reference 3.4). It has `Equal`,
`Hash` and `Order`, byte by byte, but no `Text` (reference 9.2). Mo has no
way to write a `Bytes` value; Core's literal for one lets these checks build
bytes that aren't UTF-8, which the comments write as lists of bytes. -/

/-- `text.bytes`. -/
def bytesOf (text : Expr) : Expr := top .bytes [text]

/-- `bytes.decode_utf8 == Some(text)`, or `== None` for `none`. -/
def decodesTo (bytes : Expr) : Option String → Expr
  | some text => eq (tOpt tText) (prim (.bytes .decodeUtf8) [bytes]) (some_ tText (str text))
  | none => eq (tOpt tText) (prim (.bytes .decodeUtf8) [bytes]) (none_ tText)

/-- `bytes.get(i)`. -/
def byteAt (bytes : Expr) (i : Int) : Expr := prim (.bytes .get) [bytes, int i]

-- "é€😀a".bytes == [0xC3, 0xA9, 0xE2, 0x82, 0xAC, 0xF0, 0x9F, 0x98, 0x80, 0x61],
-- and its size is the text's byte_count, 10
#guard passes program
  (lets [("b", bytesOf (str "é€😀a"))] <|
    and_ (eq tBytes (v "b") (bytesE [0xC3, 0xA9, 0xE2, 0x82, 0xAC, 0xF0, 0x9F, 0x98, 0x80, 0x61])) <|
      and_ (ieq (prim (.bytes .size) [v "b"]) (int 10))
        (ieq (prim (.bytes .size) [v "b"]) (top .byteCount [str "é€😀a"])))

-- "é".bytes.get(0) == Some(0xC3), get(1) == Some(0xA9), and get(2) and get(-1) are None
#guard passes program
  (lets [("b", bytesOf (str "é"))] <|
    and_ (eq (tOpt tByte) (byteAt (v "b") 0) (some_ tByte (byteE 0xC3))) <|
      and_ (eq (tOpt tByte) (byteAt (v "b") 1) (some_ tByte (byteE 0xA9))) <|
        and_ (eq (tOpt tByte) (byteAt (v "b") 2) (none_ tByte))
          (eq (tOpt tByte) (byteAt (v "b") (-1)) (none_ tByte)))

-- t.bytes.decode_utf8 == Some(t), and t.bytes.size == t.byte_count, for a text
-- with the characters at each end of each length of encoding: U+0000, U+007F,
-- U+0080, U+07FF, U+0800, U+D7FF, U+E000, U+FFFF, U+10000 and U+10FFFF
#guard
  let t := String.ofList ([0x0, 0x7F, 0x80, 0x7FF, 0x800, 0xD7FF, 0xE000, 0xFFFF, 0x10000, 0x10FFFF].map Char.ofNat)
  passes program
    (and_ (decodesTo (bytesOf (str t)) (some t))
      (ieq (prim (.bytes .size) [bytesOf (str t)]) (top .byteCount [str t])))

-- What isn't UTF-8 decodes to None: an overlong form ([0xC0, 0x80] and
-- [0xE0, 0x9F, 0xBF]), a surrogate ([0xED, 0xA0, 0x80]), a code above U+10FFFF
-- ([0xF4, 0x90, 0x80, 0x80]), a sequence cut short ([0xE2, 0x82], and
-- [0x61, 0xF0, 0x9F, 0x98] at the end), a continuation byte alone ([0x80]),
-- and a byte that never appears in UTF-8 ([0xFF])
#guard passes program
  (and_ (decodesTo (bytesE [0xC0, 0x80]) none) <|
    and_ (decodesTo (bytesE [0xE0, 0x9F, 0xBF]) none) <|
    and_ (decodesTo (bytesE [0xED, 0xA0, 0x80]) none) <|
    and_ (decodesTo (bytesE [0xF4, 0x90, 0x80, 0x80]) none) <|
    and_ (decodesTo (bytesE [0xE2, 0x82]) none) <|
    and_ (decodesTo (bytesE [0x61, 0xF0, 0x9F, 0x98]) none) <|
    and_ (decodesTo (bytesE [0x80]) none) (decodesTo (bytesE [0xFF]) none))

-- A byte order mark is a character like any other:
-- [0xEF, 0xBB, 0xBF, 0x61].decode_utf8 == Some("\u{FEFF}a"), and [].decode_utf8 == Some("")
#guard passes program
  (and_ (decodesTo (bytesE [0xEF, 0xBB, 0xBF, 0x61]) (some (String.ofList [Char.ofNat 0xFEFF, 'a'])))
    (decodesTo (bytesE []) (some "")))

-- "ab".bytes == [0x61, 0x62], and "ab".bytes != "ba".bytes
#guard passes program
  (and_ (eq tBytes (bytesOf (str "ab")) (bytesE [0x61, 0x62]))
    (ne tBytes (bytesOf (str "ab")) (bytesOf (str "ba"))))

-- Byte by byte: [1, 2] < [1, 2, 0] < [1, 3], [] < [0], [1, 2] <= [1, 2], and
-- not [1, 3] >= [2]; "a".bytes < "é".bytes, as "a" < "é"
#guard passes program
  (and_ (cmp .bytes .lt (bytesE [1, 2]) (bytesE [1, 2, 0])) <|
    and_ (cmp .bytes .lt (bytesE [1, 2, 0]) (bytesE [1, 3])) <|
    and_ (cmp .bytes .gt (bytesE [0]) (bytesE [])) <|
    and_ (cmp .bytes .le (bytesE [1, 2]) (bytesE [1, 2])) <|
    and_ (not_ (cmp .bytes .ge (bytesE [1, 3]) (bytesE [2]))) <|
    and_ (cmp .bytes .lt (bytesOf (str "a")) (bytesOf (str "é"))) (cmp .text .lt (str "a") (str "é")))

-- [[2], [1, 2, 0], [1, 2]].sort_by(fn(b) b end) == [[1, 2], [1, 2, 0], [2]]
#guard passes program
  (eq (tList tBytes)
    (sortByL tBytes tBytes (listOf tBytes [bytesE [2], bytesE [1, 2, 0], bytesE [1, 2]]) (lam [("b", tBytes)] (v "b")))
    (listOf tBytes [bytesE [1, 2], bytesE [1, 2, 0], bytesE [2]]))

-- Bytes are map keys: {"a".bytes: 1}.get([0x61]) == Some(1)
#guard passes program
  (eq (tOpt tInt) (mop tBytes tInt .get [mapOf tBytes tInt [(bytesOf (str "a"), int 1)], bytesE [0x61]])
    (some_ tInt (int 1)))

-- Each byte of a Bytes literal is from 0 to 255: the checker refuses 256 and -1.
#guard typedAs? program [] (bytesE [0, 255]) tBytes
#guard !typedAs? program [] (bytesE [0, 256]) tBytes
#guard !typedAs? program [] (bytesE [-1]) tBytes

/-! ### `read_bytes`

The model's world keeps each file as its text, so `read_bytes` gives the UTF-8
encoding of that text: the bytes a real file holding the text holds. Its
errors are `read_text`'s. With `files = Files.in_memory({"a.txt": "héllo\n"})`: -/

def caps : List (String × Cap) := [("files", .files)]
def world : World := { files := [("a.txt", "héllo
")] }
def notFound (path : String) : Expr := .con tFileError "NotFound" [str path]
def notAllowed (path : String) : Expr := .con tFileError "NotAllowed" [str path]
def bytesRes : Ty := tRes tBytes tFileError

-- files.read_bytes("a.txt") == Ok([0x68, 0xC3, 0xA9, 0x6C, 0x6C, 0x6F, 0x0A]),
-- which decodes to the file's text, and files.read_only.read_bytes("a.txt") is the same
#guard passes program
  (lets [("r", io (.readBytes false) [v "files", str "a.txt"])] <|
    and_ (eq bytesRes (v "r") (ok_ tBytes tFileError (bytesE [0x68, 0xC3, 0xA9, 0x6C, 0x6C, 0x6F, 0x0A]))) <|
      and_ (eq bytesRes (io (.readBytes true) [io .readOnly [v "files"], str "a.txt"]) (v "r"))
        (caseRes (v "r") "b"
          (eq (tRes tText tFileError) (io (.readText false) [v "files", str "a.txt"])
            (caseOpt (prim (.bytes .decodeUtf8) [v "b"]) "t" (ok_ tText tFileError (v "t"))
              (ok_ tText tFileError (str ""))))
          "_" ff))
  world caps

-- files.read_bytes("gone.txt") == Error(NotFound(path: "gone.txt")),
-- files.read_bytes("../a.txt") == Error(NotAllowed(path: "../a.txt")), and
-- files.within("..").read_bytes("a.txt") == Error(NotAllowed(path: "a.txt"))
#guard passes program
  (and_ (eq bytesRes (io (.readBytes false) [v "files", str "gone.txt"])
          (err_ tBytes tFileError (notFound "gone.txt"))) <|
    and_ (eq bytesRes (io (.readBytes false) [v "files", str "../a.txt"])
          (err_ tBytes tFileError (notAllowed "../a.txt")))
      (eq bytesRes (io (.readBytes false) [io (.within false) [v "files", str ".."], str "a.txt"])
        (err_ tBytes tFileError (notAllowed "a.txt"))))
  world caps

/-! ### UTF-8, against Lean's own decoder

`decode_utf8` gives what Lean's `String.fromUTF8?` gives on every sequence of
one or two bytes, every sequence of three bytes that starts with a byte from
`E0` to `EF`, and every sequence of four that starts with `F0`, `F1`, `F4` or
`F5`, then any byte, then two from `7F`, `80`, `BF` and `C0`, the bytes either
side of the continuation bytes' range. And `text.bytes` is Lean's own
encoding, `String.toUTF8`. -/

/-- Whether `utf8Decode` and Lean's `String.fromUTF8?` agree on some bytes. -/
def agreesWithLean (bs : List Int) : Bool :=
  (utf8Decode bs).map String.ofList == String.fromUTF8? ⟨(bs.map fun b => b.toNat.toUInt8).toArray⟩

def allBytes : List Int := (List.range 256).map Int.ofNat

#guard allBytes.all fun a => agreesWithLean [a]
#guard allBytes.all fun a => allBytes.all fun b => agreesWithLean [a, b]
#guard (allBytes.filter fun a => 0xE0 ≤ a && a ≤ 0xEF).all fun a => allBytes.all fun b =>
  allBytes.all fun c => agreesWithLean [a, b, c]
#guard [0xF0, 0xF1, 0xF4, 0xF5].all fun a => allBytes.all fun b =>
  [0x7F, 0x80, 0xBF, 0xC0].all fun c => [0x7F, 0x80, 0xBF, 0xC0].all fun d => agreesWithLean [a, b, c, d]

#guard ["", "a", "héllo
", "é€😀a", String.ofList [Char.ofNat 0x10FFFF, Char.ofNat 0xD7FF]].all fun t =>
  textBytes t == t.toUTF8.toList.map fun b => (b.toNat : Int)

end MoCore.Builtins
