/-
  Mo Core: checks of the built-in types `Byte`, `Bytes`, `Range` and `Set`.

  The twelve samples use these types little or not at all, so this file checks
  their primitives the way the samples check their functions: each check is a
  Core expression of type `Bool`, under the Mo it lowers from, which `#guard`
  checks is well typed and evaluates to `true` (`passes`).
-/
import MoCore.Testing
import MoCore.Text.Run

namespace MoCore.Builtins

open MoCore DSL Lib

/-- Each existing expression case also exercises both directions of the text codec. -/
def passes (p : Program) (e : Expr) (w : World := {}) (caps : List (String × Cap) := []) : Bool :=
  Text.programRoundTrip p && Text.exprRoundTrip e &&
    match Text.parseProgram (Text.printProgram p), Text.parseDocument (Text.document (Text.exprOut e)) >>= Text.exprIn with
    | .ok p', .ok e' => MoCore.passes p' e' w caps
    | _, _ => false

/-- The library functions the checks call, and the text forms the lowering
derives for the sets they write. -/
def program : Program :=
  wholeProgram "Builtins" "main" [] [] <|
    sortBy tByte tByte ++ sortBy tBytes tBytes ++
      [fn "main" [] tUnit unitE, setText tInt intText, setText (tSet tInt) (setTextL tInt),
       setText tRange fun r => prim (.range .toText) [r]]

#guard wellTyped? program

/-! ## Numeric primitives and single rounding

Use the checked run protocol and structural Float bits, rather than IEEE
equality, so signed zero, subnormal endpoints and one-ulp errors stay visible. -/

def numericReturns (t : Ty) (e : Expr) (expected : String) : Bool :=
  let p := wholeProgram "Numbers" "main" [] [] [fn "main" [] t e]
  Text.programRoundTrip p &&
    match Text.execute ⟨p, {}, .entry [], 100⟩ with
    | .returned s _ => s == expected
    | _ => false

-- This Decimal lies below 1 + 2^-53, despite its integer count rounding upward.
#guard numericReturns .float (prim (.dec .toFloat) [dec "1.000000000000000065"]) "0x3ff0000000000000"
#guard numericReturns .float (prim (.dec .toFloat) [dec "-1.000000000000000065"]) "0xbff0000000000000"

-- Both even/odd midpoint endpoints, tiny signed values, the normal/subnormal
-- transition, maximum finite and finite overflow. Expected bits are fixed.
#guard ([
  ("1.000000000000000065", "Some(0x3ff0000000000000)"),
  ("1.00000000000000011102230246251565404236316680908203125", "Some(0x3ff0000000000000)"),
  ("1.00000000000000033306690738754696212708950042724609375", "Some(0x3ff0000000000002)"),
  ("-0", "Some(0x8000000000000000)"),
  ("-1e-999999", "Some(0x8000000000000000)"),
  ("2.4703282292062327e-324", "Some(0x0000000000000000)"),
  ("2.4703282292062328e-324", "Some(0x0000000000000001)"),
  ("4.9406564584124654e-324", "Some(0x0000000000000001)"),
  ("2.225073858507201e-308", "Some(0x000fffffffffffff)"),
  ("2.2250738585072014e-308", "Some(0x0010000000000000)"),
  ("1.7976931348623157e308", "Some(0x7fefffffffffffff)"),
  ("1.7976931348623159e308", "None"), ("1e999999", "None"),
  ("0e999999", "Some(0x0000000000000000)"),
  ("12.5E+1", "Some(0x405f400000000000)"),
  ("nan", "Some(0x7ff8000000000000)"), ("inf", "Some(0x7ff0000000000000)"),
  ("-inf", "Some(0xfff0000000000000)")]).all fun (s, want) =>
    numericReturns (.option .float) (top .toFloat [str s]) want
#guard (["", " 1", "1 ", "+1", "1.", ".5", "1e", "1e+", "1e--2", "1_0", "NaN", "-nan", "0x1p0", "１２"]).all
  fun s => numericReturns (.option .float) (top .toFloat [str s]) "None"

-- Shortest-text conversion differs from rounding the exact binary 0.1.
-- The last decimal digit in the two half-place cases must round to even.
#guard ([
  (0x3fb999999999999a, "Some(0.1)"), (0x3fd5555555555555, "Some(0.3333333333333333)"),
  (0x3c3bab8cbabb6581, "Some(0.000000000000000002)"),
  (0x3c470ef54646d497, "Some(0.000000000000000002)"),
  (0x8000000000000000, "Some(0)"), (0x0000000000000001, "Some(0)"),
  (0x7ff8000000000001, "None"), (0x7ff0000000000000, "None"),
  (0xfff0000000000000, "None"), (0x7fefffffffffffff, "None")] : List (UInt64 × String)).all
  fun (bits, want) => numericReturns (.option .decimal)
    (prim (.float .toDecimal) [.lit (.float bits)]) want

-- D236 fixes layout at decimal exponents -6 and 21. Adjacent binary64 values
-- distinguish each boundary; zeros are always plain digits.
#guard ([
  (0x3fb999999999999a, "0.1"), (0x3ff8000000000000, "1.5"),
  (0x408f400000000000, "1000"), (0x4008000000000000, "3"),
  (0x0000000000000000, "0"), (0x8000000000000000, "-0"),
  (0x3eb0c6f7a0b5ed8d, "0.000001"), (0x3eb0c6f7a0b5ed8c, "9.999999999999997e-7"),
  (0xbeb0c6f7a0b5ed8d, "-0.000001"), (0xbeb0c6f7a0b5ed8c, "-9.999999999999997e-7"),
  (0x444b1ae4d6e2ef50, "1e21"), (0x444b1ae4d6e2ef4f, "999999999999999900000"),
  (0xc44b1ae4d6e2ef50, "-1e21"), (0xc44b1ae4d6e2ef4f, "-999999999999999900000"),
  (0x3e7ad7f29abcaf48, "1e-7"), (0x3e8421f5f40d8376, "1.5e-7"),
  (0x0000000000000001, "5e-324"), (0x7fefffffffffffff, "1.7976931348623157e308")]
  : List (UInt64 × String)).all fun (bits, want) =>
    numericReturns .text (prim (.float .toText) [.lit (.float bits)]) want

-- Truncation differs from IEEE remainder's nearest quotient; exceptional
-- inputs distinguish dividend/divisor handling and canonical NaN behavior.
#guard ([
  (0xc017000000000000, 0x4000000000000000, "0xbffc000000000000"),
  (0xc010000000000000, 0x4000000000000000, "0x8000000000000000"),
  (0x4017000000000000, 0xc000000000000000, "0x3ffc000000000000"),
  (0x3ff0000000000000, 0, "0x7ff8000000000000"),
  (0x7ff0000000000000, 0x4000000000000000, "0x7ff8000000000000"),
  (0xbff0000000000000, 0x7ff0000000000000, "0xbff0000000000000"),
  (0x3ff0000000000000, 0x7ff8000000000001, "0x7ff8000000000000")]
  : List (UInt64 × UInt64 × String)).all fun (a, b, want) =>
    numericReturns .float (prim (.float .rem) [.lit (.float a), .lit (.float b)]) want

def numberChecks : Expr :=
  let big (n : Int) := Expr.lit (.bigInt n)
  let bop (o : BigOp) (xs : List Expr) := prim (.big o) xs
  and_ (eq .bigInt (bop .min [big (-7), big 13]) (big (-7))) <|
  and_ (eq .bigInt (bop .max [big (-7), big 13]) (big 13)) <|
  and_ (eq .bigInt (bop .pow [big (-3), int 41]) (big (-36472996377170786403))) <|
  and_ (eq .bigInt (bop .pow [big 0, int 0]) (big 1)) <|
  and_ (eq .bigInt (bop .floorDiv [big (-7), big 3]) (big (-3))) <|
  and_ (eq .bigInt (bop .floorMod [big (-7), big 3]) (big 2)) <|
  and_ (eq .bigInt (bop .floorDiv [big 7, big (-3)]) (big (-3))) <|
  and_ (eq .bigInt (bop .floorMod [big 7, big (-3)]) (big (-2))) <|
  and_ (eq (tOpt .float) (top .toFloat [str "1.000000000000000065"])
    (some_ .float (.lit (.float 0x3ff0000000000000)))) <|
  and_ (eq (tOpt .decimal) (prim (.float .toDecimal) [.lit (.float 0x3fb999999999999a)])
    (some_ .decimal (dec "0.1"))) <|
  eq .float (prim (.float .rem) [.lit (.float 0x4017000000000000), .lit (.float 0x4000000000000000)])
    (.lit (.float 0x3ffc000000000000))

#guard passes program numberChecks
#guard stops program (prim (.big .pow) [.lit (.bigInt 2), int (-1)]) .bigInt
  (.outside "BigInt.pow with a negative exponent")
#guard [BigOp.floorDiv, .floorMod].all fun op =>
  stops program (prim (.big op) [.lit (.bigInt 7), .lit (.bigInt 0)]) .bigInt .divByZero
#guard !typedAs? program [] (prim (.big .pow) [.lit (.bigInt 2), .lit (.bigInt 3)]) .bigInt

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

/-! ## Range

`a..b` is a value of type `Range`: the whole numbers from `a` up to, not
including, `b` (reference 7.5). It has `Equal`, `Hash` and a text form, `0..3`,
but no order (reference 9.2). -/

/-- `r.to_list`. -/
def toList (r : Expr) : Expr := prim (.range .toList) [r]

/-- `r.contains?(n)`. -/
def contains (r n : Expr) : Expr := prim (.range .contains) [r, n]

-- (0..3).to_list == [0, 1, 2], (-2..1).to_list == [-2, -1, 0], and
-- (3..3).to_list and (5..2).to_list are []
#guard passes program
  (and_ (eq (tList tInt) (toList (range (int 0) (int 3))) (listOf tInt [int 0, int 1, int 2])) <|
    and_ (eq (tList tInt) (toList (range (iop .neg [int 2]) (int 1)))
      (listOf tInt [iop .neg [int 2], iop .neg [int 1], int 0])) <|
    and_ (eq (tList tInt) (toList (range (int 3) (int 3))) (nil tInt))
      (eq (tList tInt) (toList (range (int 5) (int 2))) (nil tInt)))

-- With r = 0..3: r.contains?(0) and r.contains?(2), but not r.contains?(3) or
-- r.contains?(-1); and (5..2).contains?(3) is false
#guard passes program
  (lets [("r", range (int 0) (int 3))] <|
    and_ (contains (v "r") (int 0)) <|
    and_ (contains (v "r") (int 2)) <|
    and_ (not_ (contains (v "r") (int 3))) <|
    and_ (not_ (contains (v "r") (iop .neg [int 1])))
      (not_ (contains (range (int 5) (int 2)) (int 3))))

-- (0..3).to_text == "0..3", and "#{-2..10}" == "-2..10"
#guard passes program
  (and_ (teq (prim (.range .toText) [range (int 0) (int 3)]) (str "0..3"))
    (teq (prim (.range .toText) [range (iop .neg [int 2]) (int 10)]) (str "-2..10")))

-- Two ranges are equal when their ends are: 0..3 == 0..3 and 0..3 != 0..4,
-- and 5..2 != 6..2, though both are empty
#guard passes program
  (and_ (eq tRange (range (int 0) (int 3)) (range (int 0) (int 3))) <|
    and_ (ne tRange (range (int 0) (int 3)) (range (int 0) (int 4)))
      (ne tRange (range (int 5) (int 2)) (range (int 6) (int 2))))

-- Ranges are map keys: {0..3: "short", 0..10: "long"}.get(0..10) == Some("long")
#guard passes program
  (eq (tOpt tText)
    (mop tRange tText .get [mapOf tRange tText [(range (int 0) (int 3), str "short"),
      (range (int 0) (int 10), str "long")], range (int 0) (int 10)])
    (some_ tText (str "long")))

/-- ```
var total = 0
for i in 0..10
  continue if i % 2 == 0
  total += i
end
total == 25
``` -/
def oddSum : Expr :=
  lets [("total", int 0)] <|
    forRange "r" "i" (range (int 0) (int 10)) [("total", tInt)] [v "total"]
      (ite (ieq (irem (v "i") (int 2)) (int 0)) (nextRange "r" "i" [v "total"])
        (nextRange "r" "i" [iadd (v "total") (v "i")]))
      (ieq (v "total") (int 25))

#guard passes program oddSum

/-- ```
r = start..9223372036854775807
var count = 0
for i in r
  count += 1
end
count
```
A loop up to the largest `Int` counts to its end without overflow: the counter
is below the end whenever it is increased. -/
def countTo (start : Expr) : Expr :=
  lets [("r", range start (int int64Max)), ("count", int 0)] <|
    forRange "r" "i" (v "r") [("count", tInt)] [v "count"]
      (nextRange "r" "i" [iadd (v "count") (int 1)])
      (v "count")

-- countTo(9223372036854775804) == 3, and an empty range runs no round: countTo(9223372036854775807) == 0
#guard passes program (ieq (countTo (int (int64Max - 3))) (int 3))
#guard passes program (ieq (countTo (int int64Max)) (int 0))

/-! ## Set

`Set(T)` holds distinct values in insertion order, also after removals
(reference 3.8). It has `Equal`, comparing elements in any order, `Hash`, so
a set can be a map key or a set member, and a text form, `[1, 2].to_set`, but
no order (reference 9.2). -/

/-- `[n1, ..., nk].to_set`, a set of `Int`s. -/
def intSet (ns : List Int) : Expr := sop tInt .ofList [listOf tInt (ns.map int)]

/-- `s.to_list == [n1, ..., nk]`: the set of `Int`s `s` holds these, in this
order. -/
def holdsInOrder (s : Expr) (ns : List Int) : Expr :=
  eq (tList tInt) (sop tInt .toList [s]) (listOf tInt (ns.map int))

-- Set.empty.size == 0, Set.empty.empty?, and Set.empty.to_list == []
#guard passes program
  (and_ (ieq (sop tInt .size [emptySet tInt]) (int 0)) <|
    and_ (sop tInt .isEmpty [emptySet tInt]) (holdsInOrder (emptySet tInt) []))

-- A new element goes last: s = Set.empty.add(3).add(1).add(2) holds [3, 1, 2];
-- one already there keeps its place: s.add(3) holds [3, 1, 2], and
-- s.add(1).size == 3
#guard passes program
  (lets [("s", sop tInt .add [sop tInt .add [sop tInt .add [emptySet tInt, int 3], int 1], int 2])] <|
    and_ (holdsInOrder (v "s") [3, 1, 2]) <|
      and_ (holdsInOrder (sop tInt .add [v "s", int 3]) [3, 1, 2])
        (ieq (sop tInt .size [sop tInt .add [v "s", int 1]]) (int 3)))

-- s = [3, 1, 3, 2, 1].to_set holds [3, 1, 2], each element in its first place,
-- as [3, 1, 3, 2, 1].unique gives them; s.size == 3, and not s.empty?
#guard passes program
  (lets [("s", intSet [3, 1, 3, 2, 1])] <|
    and_ (holdsInOrder (v "s") [3, 1, 2]) <|
    and_ (eq (tList tInt) (sop tInt .toList [v "s"])
      (lop tInt .unique [listOf tInt ([3, 1, 3, 2, 1].map int)])) <|
    and_ (ieq (sop tInt .size [v "s"]) (int 3)) (not_ (sop tInt .isEmpty [v "s"])))

-- ["b", "a", "b"].to_set.to_list == ["b", "a"], and it has?("a")
#guard passes program
  (lets [("s", sop tText .ofList [strs ["b", "a", "b"]])] <|
    and_ (eq (tList tText) (sop tText .toList [v "s"]) (strs ["b", "a"]))
      (sop tText .has [v "s", str "a"]))

-- With s = [3, 1, 2].to_set: s.has?(1), and not s.has?(4)
#guard passes program
  (lets [("s", intSet [3, 1, 2])] <|
    and_ (sop tInt .has [v "s", int 1]) (not_ (sop tInt .has [v "s", int 4])))

-- remove keeps the others' order: with s = [3, 1, 2].to_set, s.remove(1)
-- holds [3, 2] and s.remove(4) holds [3, 1, 2]; an element removed and added
-- again goes last: s.remove(3).add(3) holds [1, 2, 3]
#guard passes program
  (lets [("s", intSet [3, 1, 2])] <|
    and_ (holdsInOrder (sop tInt .remove [v "s", int 1]) [3, 2]) <|
    and_ (holdsInOrder (sop tInt .remove [v "s", int 4]) [3, 1, 2])
      (holdsInOrder (sop tInt .add [sop tInt .remove [v "s", int 3], int 3]) [1, 2, 3]))

-- union keeps the first set's elements in their order, then adds the second's
-- that are new, in theirs: [3, 1].to_set.union([2, 1, 4].to_set) holds
-- [3, 1, 2, 4], [2, 1, 4].to_set.union([3, 1].to_set) holds [2, 1, 4, 3], and
-- Set.empty.union([2, 1].to_set) holds [2, 1]
#guard passes program
  (and_ (holdsInOrder (sop tInt .union [intSet [3, 1], intSet [2, 1, 4]]) [3, 1, 2, 4]) <|
    and_ (holdsInOrder (sop tInt .union [intSet [2, 1, 4], intSet [3, 1]]) [2, 1, 4, 3])
      (holdsInOrder (sop tInt .union [emptySet tInt, intSet [2, 1]]) [2, 1]))

-- intersection and difference keep the first set's order:
-- [3, 1, 2].to_set.intersection([2, 3, 5].to_set) holds [3, 2],
-- [3, 1, 2].to_set.difference([2, 5].to_set) holds [3, 1], and with
-- Set.empty, they hold [] and [3, 1, 2]
#guard passes program
  (and_ (holdsInOrder (sop tInt .intersection [intSet [3, 1, 2], intSet [2, 3, 5]]) [3, 2]) <|
    and_ (holdsInOrder (sop tInt .difference [intSet [3, 1, 2], intSet [2, 5]]) [3, 1]) <|
    and_ (holdsInOrder (sop tInt .intersection [intSet [3, 1, 2], emptySet tInt]) [])
      (holdsInOrder (sop tInt .difference [intSet [3, 1, 2], emptySet tInt]) [3, 1, 2]))

-- Two sets are equal when they hold the same elements, in any order:
-- [1, 2].to_set == [2, 1].to_set and Set.empty == [].to_set, but
-- [1, 2].to_set != [1, 2, 3].to_set and [1, 2].to_set != [1, 3].to_set
#guard passes program
  (and_ (eq (tSet tInt) (intSet [1, 2]) (intSet [2, 1])) <|
    and_ (eq (tSet tInt) (emptySet tInt) (intSet [])) <|
    and_ (ne (tSet tInt) (intSet [1, 2]) (intSet [1, 2, 3]))
      (ne (tSet tInt) (intSet [1, 2]) (intSet [1, 3])))

-- Sets are map keys and set members, equal sets being the same key:
-- {[1, 2].to_set: "a"}.get([2, 1].to_set) == Some("a"), and
-- [[1, 2].to_set, [2, 1].to_set, Set.empty].to_set.size == 2
#guard passes program
  (and_ (eq (tOpt tText)
      (mop (tSet tInt) tText .get [mapOf (tSet tInt) tText [(intSet [1, 2], str "a")], intSet [2, 1]])
      (some_ tText (str "a")))
    (ieq (sop (tSet tInt) .size
      [sop (tSet tInt) .ofList [listOf (tSet tInt) [intSet [1, 2], intSet [2, 1], emptySet tInt]]])
      (int 2)))

-- An element equal to one already there leaves the set as it is, with the
-- element it held: [{1: "a", 2: "b"}].to_set.add({2: "b", 1: "a"}) holds one
-- map, whose keys are still [1, 2]
#guard
  let tM := tMap tInt tText
  let m := mapOf tInt tText [(int 1, str "a"), (int 2, str "b")]
  let m2 := mapOf tInt tText [(int 2, str "b"), (int 1, str "a")]
  passes program
    (lets [("s", sop tM .add [sop tM .ofList [listOf tM [m]], m2])] <|
      and_ (ieq (sop tM .size [v "s"]) (int 1))
        (caseOpt (lop tM .first [sop tM .toList [v "s"]]) "m"
          (eq (tList tInt) (mop tInt tText .keys [v "m"]) (listOf tInt [int 1, int 2])) ff))

-- "#{[3, 1, 2].to_set}" == "[3, 1, 2].to_set" and Set.empty.to_text == "[].to_set";
-- [2, 1].to_set == [1, 2].to_set, but each is written in its own order:
-- "[2, 1].to_set" and "[1, 2].to_set"
#guard passes program
  (and_ (teq (setTextL tInt (intSet [3, 1, 2])) (str "[3, 1, 2].to_set")) <|
    and_ (teq (setTextL tInt (emptySet tInt)) (str "[].to_set")) <|
    and_ (teq (setTextL tInt (intSet [2, 1])) (str "[2, 1].to_set"))
      (teq (setTextL tInt (intSet [1, 2])) (str "[1, 2].to_set")))

-- Inside a set, a set or a range is written as it is alone:
-- [[1, 2].to_set, Set.empty].to_set.to_text == "[[1, 2].to_set, [].to_set].to_set",
-- and [0..3, -2..1].to_set.to_text == "[0..3, -2..1].to_set"
#guard passes program
  (and_ (teq (setTextL (tSet tInt)
        (sop (tSet tInt) .ofList [listOf (tSet tInt) [intSet [1, 2], emptySet tInt]]))
      (str "[[1, 2].to_set, [].to_set].to_set"))
    (teq (setTextL tRange (sop tRange .ofList
        [listOf tRange [range (int 0) (int 3), range (iop .neg [int 2]) (int 1)]]))
      (str "[0..3, -2..1].to_set")))

/-- ```
var order = []
for n in [3, 1, 3, 2, 4].to_set
  continue if n == 1
  break if n == 4
  order = order.append(n)
end
order == [3, 2]
```
A `for` loop visits a set's elements in insertion order. -/
def setLoop : Expr :=
  lets [("order", nil tInt)] <|
    forSet "l" tInt "n" (intSet [3, 1, 3, 2, 4]) [("order", tList tInt)] [v "order"]
      (ite (ieq (v "n") (int 1)) (nextRound "l" [v "order"]) <|
        ite (ieq (v "n") (int 4)) (breakLoop "l" [v "order"]) <|
          nextRound "l" [lop tInt .append [v "order", v "n"]])
      (eq (tList tInt) (v "order") (listOf tInt [int 3, int 2]))

#guard passes program setLoop

-- A set is a type of its own, not a map to Unit: the checker refuses
-- Set.empty where a Map(Int, Unit) is expected, and a Text added to a Set(Int).
#guard typedAs? program [] (emptySet tInt) (tSet tInt)
#guard !typedAs? program [] (emptySet tInt) (tMap tInt tUnit)
#guard !typedAs? program [] (sop tInt .add [emptySet tInt, str "a"]) (tSet tInt)

/-! ## Failure is not a stop -/

#guard typedAs? program [] (DSL.fail (cat [str "left: ", intText (int 7)])) tUnit true
#guard !typedAs? program [] (DSL.fail (int 7)) tUnit true
#guard match run program (DSL.fail (cat [str "left: ", intText (int 7)])) with
  | .failed "left: 7" _ => true
  | _ => false

-- Effects in the detail survive, and a failure skips the continuation.
#guard match run program
    (seq (DSL.fail (seq (print (v "console") (str "before")) (str "detail")))
      (print (v "console") (str "after"))) {} (handles [("console", .console)]) with
  | .failed "detail" w => w.stdout == ["before"]
  | _ => false

-- A bug while computing the detail stays a bug, not an assertion failure.
#guard stops program (DSL.fail (stop "detail stopped")) tUnit (.stop "detail stopped")

/-! ## Program initialization and the spec boundary -/

def layered : Program :=
  { data := []
    consts := [const "seed" tInt (int 7),
      const "twice" tInt (iadd (call "first" []) (global "seed")),
      const "saved" (tFn [] tInt) (lam [] (global "twice")),
      const "last" tInt (int 99)]
    entry := "start"
    funs := [
      { (fn "first" [] tInt (global "seed")) with module := "Library", constCount := 1 },
      { (fn "start" [("console", tCap .console)] tInt
          (seq (print (v "console") (intText (global "twice")))
            (lets [("twice", int 999)] (app (global "saved") [])))) with
          module := "Cli", constCount := 4 },
      { (fn "bad assertion" [] tUnit (app (lam [] (assertThat ff (str "expected 14"))) [])) with
          module := "Spec", kind := .test, constCount := 4 },
      { (fn "good example" [] tUnit (assertThat (ieq (global "twice") (int 14)) (str "twice"))) with
          module := "Spec", kind := .example, constCount := 4 },
      { (promiseFn "bounded" [("result", tInt)] [igt (v "result") (int 0), ilt (v "result") (int 10)]) with
          module := "Spec", constCount := 4 },
      { (fn "forbids" [] tInt (call "bounded" [int 20])) with
          module := "Spec", kind := .anchor, constCount := 4 }] }

#guard wellTyped? layered
#guard match runProgram 30 layered [.cap .console (some "")] with
  | .ok (.int 14) w => w.stdout == ["14"]
  | _ => false
#guard match runTest 30 layered "bad assertion" with
  | .failed "expected 14" _ => true
  | _ => false
#guard match runTest 30 layered "good example" with
  | .ok .unit _ => true
  | _ => false
#guard passes layered (ieq (call "forbids" []) (int 2))
#guard passes layered (ieq (call "bounded" [int (-1)]) (int 1))
#guard passes layered (ieq (call "bounded" [int 5]) (int 0))
#guard match runTest 30 layered "forbids" with
  | .wrong _ => true
  | _ => false
#guard match runTest 30 layered "missing" with
  | .wrong _ => true
  | _ => false
#guard match runProgram 0 layered [.cap .console (some "")] with
  | .outOfFuel => true
  | _ => false

-- Checking is lexical: anonymous bodies inherit their enclosing spec flag,
-- including named functions used as values (a lambda calling the name).
#guard !typedAs? layered [] (DSL.fail (str "no")) tUnit
#guard !typedAs? layered [] (call "bad assertion" []) tUnit
#guard !typedAs? layered [] (lam [] (call "bad assertion" [])) (tFn [] tUnit)
#guard !typedAs? layered [] (lam [] (DSL.fail (str "no"))) (tFn [] tUnit)
#guard typedAs? layered [] (lam [] (call "bad assertion" [])) (tFn [] tUnit) true
#guard !typedAs? layered [("c", tCap .console)] (io .printed [v "c"]) (tList tText)
#guard !typedAs? layered [("c", tCap .console)] (lam [] (io .printedErrors [v "c"]))
  (tFn [] (tList tText))
#guard typedAs? layered [("c", tCap .console)] (io .printedErrors [v "c"]) (tList tText) true

-- Reject an ordinary function that tries to delegate to spec code, bad kind
-- signatures, a spec entry, and a missing entry.
#guard !wellTyped? { layered with funs := layered.funs ++
  [{ (fn "ordinary" [] tUnit (call "bad assertion" [])) with constCount := 4 }] }
#guard !wellTyped? { layered with entry := "good example" }
#guard !wellTyped? { layered with entry := "missing" }
#guard [FunKind.test, .example, .anchor, .promise].all fun kind =>
  !wellTyped? { layered with funs := layered.funs ++ [{ (fn "invalid" [] tBool tt) with kind }] }
#guard !wellTyped? { layered with funs := layered.funs ++
  [{ (fn "invalid" [("x", tInt)] tUnit unitE) with kind := .test }] }
#guard !wellTyped? { layered with funs := layered.funs ++
  [{ (fn "invalid" [] tInt (int 0)) with kind := .promise }] }

-- Initializers are ordinary code. Direct and transitive forward reads, self
-- reads, duplicate constant names and falsely small function prefixes fail.
#guard !wellTyped? { layered with consts := [const "seed" tInt (global "twice")] ++ layered.consts.tail }
#guard !wellTyped? { layered with consts := [const "seed" tInt (call "first" [])] ++ layered.consts.tail }
#guard !wellTyped? { layered with consts := [const "seed" tInt (global "seed")] ++ layered.consts.tail }
#guard !wellTyped? { layered with consts := layered.consts.take 3 ++ [const "seed" tInt (int 8)] }
#guard !wellTyped? { layered with consts := [const "seed" tInt (str "wrong type")] ++ layered.consts.tail }
#guard !wellTyped? { layered with funs :=
  [{ (fn "first" [] tInt (global "seed")) with constCount := 0 }] ++ layered.funs.tail }
#guard !wellTyped? { layered with consts := [const "seed" tInt (DSL.fail (str "no"))] ++ layered.consts.tail }

def stoppedInit : Program :=
  { layered with consts := [const "seed" tInt (stop "initialization stopped")] ++ layered.consts.tail }
#guard wellTyped? stoppedInit
#guard match runProgram 30 stoppedInit [.cap .console (some "")] with
  | .stopped (.stop "initialization stopped") w => w.stdout.isEmpty
  | _ => false
#guard match runTest 30 stoppedInit "bad assertion" with
  | .stopped (.stop "initialization stopped") _ => true
  | _ => false

-- The initializer API can reuse one computed prefix across several calls.
-- A supplied console prefix makes its once-only effect observable without
-- putting spec-only constructors in constants.
#guard match initConsts 30 layered [const "message" tText
    (seq (print (global "console") (str "once")) (str "cached"))]
    [("console", .cap .console (some ""))] {} fun gs w =>
      evalWithGlobals 30 layered gs [] w (tup [global "message", global "message"]) with
  | .ok (.tuple [.text "cached", .text "cached"]) w => w.stdout == ["once"]
  | _ => false

/-! ## Independent fake capabilities -/

def fakeChecks : Expr :=
  lets [("a", io .consoleFake []), ("b", io .consoleFakeInput [strs ["input"]]),
    ("initial", mapOf tText tText [(str "a/note", str "old")]),
    ("files", io .filesInMemory [v "initial"]),
    ("other", io .filesInMemory [mop tText tText .empty []]),
    ("sub", io (.within false) [v "files", str "a"]),
    ("reader", io .readOnly [v "files"]),
    ("args1", io .argsOf [strs ["a", ""]]), ("args2", io .argsOf [strs ["b"]]),
    ("env1", io .envOf [mapOf tText tText [(str "K", str "left")]]),
    ("env2", io .envOf [mapOf tText tText [(str "K", str "right")]]),
    ("clock1", io .clockFixed [timeE "1970-01-01T00:00:00Z"]),
    ("clock2", io .clockFixed [timeE "1970-01-01T00:00:00.000123Z"]),
    ("rng1", io .randomSeeded [int (-1)]), ("rng2", io .randomSeeded [int (-1)]),
    ("draw", lam [] (io .randomInt [v "rng1", int 0, int 9223372036854775807])),
    ("first", app (v "draw") []), ("second", app (v "draw") [])] <|
  seq (io .print [v "a", str "first"]) <|
  lets [("snapshot", io .printed [v "a"])] <|
  seq (io .print [v "a", str "second"]) <|
  seq (io .printError [v "b", str "error"]) <|
  seq (call "fake.write" [v "sub"]) <|
  and_ (eq (tMap tText tText) (v "initial") (mapOf tText tText [(str "a/note", str "old")])) <|
  and_ (eq (tList tText) (v "snapshot") (strs ["first"])) <|
  and_ (eq (tList tText) (io .printed [v "a"]) (strs ["first", "second"])) <|
  and_ (eq (tList tText) (io .printed [v "b"]) (strs [])) <|
  and_ (eq (tList tText) (io .printedErrors [v "b"]) (strs ["error"])) <|
  and_ (eq (tOpt tText) (io .readLine [v "b"]) (some_ tText (str "input"))) <|
  and_ (eq (tOpt tText) (io .readLine [v "b"]) (none_ tText)) <|
  and_ (eq (tRes tText tFileError) (io (.readText true) [v "reader", str "a/note"])
    (ok_ tText tFileError (str "saved"))) <|
  and_ (eq tBool (io (.«exists» false) [v "other", str "a/note"]) ff) <|
  and_ (eq (tList tText) (argsList (v "args1")) (strs ["a", ""])) <|
  and_ (eq (tList tText) (argsList (v "args2")) (strs ["b"])) <|
  and_ (eq (tOpt tText) (io .envGet [v "env1", str "K"]) (some_ tText (str "left"))) <|
  and_ (eq (tOpt tText) (io .envGet [v "env2", str "K"]) (some_ tText (str "right"))) <|
  and_ (eq tTime (io .now [v "clock1"]) (timeE "1970-01-01T00:00:00Z")) <|
  and_ (eq tTime (io .now [v "clock2"]) (timeE "1970-01-01T00:00:00.000123Z")) <|
  and_ (not_ (ieq (v "first") (v "second"))) <|
  and_ (ieq (v "first") (io .randomInt [v "rng2", int 0, int 9223372036854775807])) <|
  ieq (v "second") (io .randomInt [v "rng2", int 0, int 9223372036854775807])

def fakeProgram : Program :=
  { program with funs := program.funs ++
    [fn "fake.write" [("files", tCap .files)] (tRes tUnit tFileError)
       (io .writeText [v "files", str "note", str "saved"]),
     { (fn "fakes" [] tUnit (assertThat fakeChecks (str "fake isolation failed"))) with kind := .test }] }

#guard wellTyped? fakeProgram
#guard passes fakeProgram fakeChecks

-- All constructors are lexical spec operations, including inside lambdas.
def fakeExpressions : List (Expr × Ty) :=
  [(io .consoleFake [], tCap .console), (io .consoleFakeInput [strs []], tCap .console),
   (io .filesInMemory [mapOf tText tText []], tCap .files),
   (io .clockFixed [timeE "2026-10-12T09:00:00Z"], tCap .clock),
   (io .randomSeeded [int (-1)], tCap .random),
   (io .argsOf [strs []], tCap .args), (io .envOf [mapOf tText tText []], tCap .env)]
#guard fakeExpressions.all fun (e, t) =>
  typedAs? program [] e t true && !typedAs? program [] e t &&
  typedAs? program [] (lam [] e) (tFn [] t) true && !typedAs? program [] (lam [] e) (tFn [] t) &&
  !wellTyped? { program with consts := [const "fake" t e] } &&
  !wellTyped? { program with funs := program.funs ++ [fn "ordinary.fake" [] t e] } &&
  [FunKind.test, .example, .promise, .anchor].all fun kind =>
    wellTyped? { program with funs := program.funs ++
      [{ (fn "spec.fake" (if kind = .promise then [("result", tUnit)] else [])
           (if kind = .test || kind = .example then tUnit else tInt)
           (seq e (if kind = .test || kind = .example then unitE else int 0))) with kind }] }

-- Allocation respects existing states; none of the fakes changes real fields.
#guard
  let host : World :=
    { args := ["host"], env := [("K", "host")], stdin := ["host"],
      stdout := ["real"], stderr := ["real error"], files := [("a/note", "real file")],
      now := 999, random := RandomState.seeded 123, fakes := [{ stdout := ["existing"] }] }
  match run fakeProgram fakeChecks host with
  | .ok (.bool true) w => reprStr w.toCapState == reprStr host.toCapState &&
      w.fakes.length == 13 && reprStr w.fakes.head? == reprStr host.fakes.head?
  | _ => false
#guard stops program (io .printed [v "c"]) (tList tText)
  (.outside "console observation needs a fake") {} [("c", .console)]

-- Int.to_char admits scalar values on both sides of the surrogate gap.
#guard
  [(-1, none_ tChar), (0xD7FF, some_ tChar (chr (Char.ofNat 0xD7FF))),
   (0xD800, none_ tChar), (0xDFFF, none_ tChar),
   (0xE000, some_ tChar (chr (Char.ofNat 0xE000))),
   (0x10FFFF, some_ tChar (chr (Char.ofNat 0x10FFFF))),
   (0x110000, none_ tChar), (int64Max, none_ tChar)].all fun (n, expected) =>
    passes program (eq (tOpt tChar) (prim (.int .toChar) [int n]) expected)

/-! ## Library surface and reference random vectors

Fixed outputs from Blackman/Vigna's published C code, compiled independently;
see TRANSLATION.md for URLs and reproduction. The four state words are also
the first four SplitMix64 outputs, not values generated by this test. -/
def randomVectors : List (Int × RandomState × List UInt64) :=
  [(0, ⟨0xe220a8397b1dcdaf, 0x6e789e6aa1b965f4, 0x06c45d188009454f, 0xf88bb8a8724c81ec⟩,
    [0x99ec5f36cb75f2b4, 0xbf6e1f784956452a, 0x1a5f849d4933e6e0,
     0x6aa594f1262d2d2c, 0xbba5ad4a1f842e59, 0xffef8375d9ebcaca]),
   (1, ⟨0x910a2dec89025cc1, 0xbeeb8da1658eec67, 0xf893a2eefb32555e, 0x71c18690ee42c90b⟩,
    [0xb3f2af6d0fc710c5, 0x853b559647364cea, 0x92f89756082a4514,
     0x642e1c7bc266a3a7, 0xb27a48e29a233673, 0x24c123126ffda722]),
   (-1, ⟨0xe4d971771b652c20, 0xe99ff867dbf682c9, 0x382ff84cb27281e9, 0x6d1db36ccba982d2⟩,
    [0x8f5520d52a7ead08, 0xc476a018caa1802d, 0x81de31c0d260469e,
     0xbf658d7e065f3c2f, 0x913593fda1bca32a, 0xbb535e93941ba525]),
   (int64Min, ⟨0x481ec0a212a9f3db, 0xc46fa638a6309012, 0x61a685ffc80a8140, 0x592e268383e356f9⟩,
    [0xd01bfa9b44a998c3, 0x797c6b72ff690d62, 0x4576af98398380b1,
     0xe5ce401830aaa16c, 0xa5eeccc1d5d5ee1a, 0xcd43606b62171d67]),
   (int64Max, ⟨0x2a67d7552e039ea7, 0xf20c01408082f947, 0xec159351af424190, 0x2020319894995bfb⟩,
    [0x0e1c2b4b82e8c0c5, 0x19167a27a6e0d81b, 0x7b5f1a55d35896bd,
     0x0d19f02bf9005c90, 0x0eee111b5f85aca0, 0xbb969c534267cf4f])]

#guard randomVectors.all fun (seed, state, expected) =>
  RandomState.seeded seed == state &&
  ((List.range 6).foldl (fun (acc, s) _ =>
    let (draw, s') := s.next
    (acc ++ [draw], s')) ([], state)).1 == expected

/-- Calls through the public fake constructor and Random.int, in order. -/
def seededDraws (seed lo hi : Int) (count : Nat) : Expr :=
  lets [("rng", io .randomSeeded [int seed])] <|
    listOf tInt ((List.range count).map fun _ => io .randomInt [v "rng", int lo, int hi])

def randomChecks : Expr :=
  [ (0, -5, 5, [-5+6, -5+7, -5+1, -5+4]),
    (1, 0, 3, [2, 1, 1, 1]),
    (-1, -9, 1, [-4, -2, -4, -2]),
    (int64Min, 0, 10, [8, 4, 2, 8]),
    (int64Max, 0, 10, [0, 0, 4, 0]),
    (0, int64Min, int64Min + 1, [int64Min, int64Min, int64Min, int64Min]),
    (0, int64Max - 1, int64Max, [int64Max - 1, int64Max - 1, int64Max - 1, int64Max - 1]),
    -- Span 2^63+1: seed 1 consumes 2, 1, 1, 4 raw draws (Lemire reference).
    (1, -2, int64Max, [4800180567299270259, 5295190459760845448,
      3609369285294772689, 3515805966490203212]),
    -- Maximum expressible half-open Int interval: span 2^64-1, no signed subtraction overflow.
    (0, int64Min, int64Max, [1867972634398290611, 4570625273314559273,
      -7322988658008267041, -1538659934228632277])].foldr
    (fun (seed, lo, hi, expected) rest =>
      and_ (eq (tList tInt) (seededDraws seed lo hi expected.length)
        (listOf tInt (expected.map int))) rest) tt

def entryE (name kind : String) : Expr :=
  .con .entry "Entry" [str name, .con .entryKind kind []]

def entryChecks : Expr :=
  let result := tRes (tList .entry) tFileError
  lets [("files", io .filesInMemory [mapOf tText tText
    [(str "z", str "last"), (str "a/β", str "beta"), (str "a/deep/n", str "nested"),
     (str "a/A", str "first"), (str "a/deep/m", str "nested again")]]),
    ("read", io .readOnly [io (.within false) [v "files", str "a"]]),
    ("snapshot", io (.entries false) [v "files", str "."])] <|
  and_ (eq result (v "snapshot")
    (ok_ (tList .entry) tFileError (listOf .entry [entryE "a" "Folder", entryE "z" "File"]))) <|
  and_ (eq result (io (.entries true) [v "read", str "."])
    (ok_ (tList .entry) tFileError (listOf .entry [entryE "A" "File", entryE "deep" "Folder", entryE "β" "File"]))) <|
  and_ (eq result (io (.entries true) [v "read", str "deep"])
    (ok_ (tList .entry) tFileError (listOf .entry [entryE "m" "File", entryE "n" "File"]))) <|
  and_ (eq result (io (.entries true) [v "read", str "../z"])
    (err_ (tList .entry) tFileError (.con tFileError "NotAllowed" [str "../z"]))) <|
  and_ (eq result (io (.entries false) [v "files", str "gone"])
    (err_ (tList .entry) tFileError (.con tFileError "NotFound" [str "gone"]))) <|
  seq (io .remove [v "files", str "z"]) <|
  and_ (eq result (v "snapshot")
    (ok_ (tList .entry) tFileError (listOf .entry [entryE "a" "Folder", entryE "z" "File"]))) <|
  eq result (io (.entries false) [v "files", str "."])
    (ok_ (tList .entry) tFileError (listOf .entry [entryE "a" "Folder"]))

/-- Every Result-returning file operation, including ReadFiles variants. -/
def fileCalls : List (IoOp × Ty × List Expr) :=
  [(.readText false, tText, []), (.readText true, tText, []),
   (.readBytes false, tBytes, []), (.readBytes true, tBytes, []),
   (.writeText, tUnit, [str "new"]), (.appendText, tUnit, [str "new"]), (.remove, tUnit, []),
   (.makeFolder, tUnit, []),
   (.list false, tList tText, []), (.list true, tList tText, []),
   (.entries false, tList .entry, []), (.entries true, tList .entry, [])]

/-- Run the same contract through real and fake handles. Failed writes/removes
must leave both the leaf and its sibling/descendant files intact. -/
def fileKindChecks : Expr :=
  let failures := fileCalls.foldr (fun (op, t, extra) rest =>
    let ro := match op with
      | .readText ro | .readBytes ro | .list ro | .entries ro => ro
      | _ => false
    let h := if ro then v "read" else v "files"
    let call (h : Expr) (path : String) := io op ([h, str path] ++ extra)
    let failure (h : Expr) (path reason : String) :=
      eq (tRes t tFileError) (call h path)
        (err_ t tFileError (.con tFileError "Failed" [str path, str reason]))
    let folderOp := match op with | .list _ | .entries _ | .makeFolder => true | _ => false
    and_ (failure h (if folderOp then "leaf" else "dir")
      (if folderOp then "not a folder" else "is a folder")) <|
    and_ (if folderOp then tt else failure h "." "is a folder") <|
    and_ (failure (io (.within ro) [h, str "leaf"]) "." "not a folder") <|
    and_ (failure (io (.within ro) [h, str "leaf"]) "child" "not a folder") <|
    and_ (failure h "leaf/child" "not a folder") <|
    and_ (eq (tRes t tFileError) (call (io (.within ro) [h, str "absent"]) "child")
      (err_ t tFileError (notFound "child"))) <|
    and_ (if op == .makeFolder then tt else
      eq (tRes t tFileError) (call h "absent/child") (err_ t tFileError (notFound "absent/child"))) <|
    and_ (eq (tRes t tFileError) (call h "../leaf")
      (err_ t tFileError (notAllowed "../leaf"))) rest) tt
  lets [("read", io .readOnly [v "files"])] <|
  and_ failures <|
  and_ (eq (tRes tText tFileError) (io (.readText true) [v "read", str "leaf"])
    (ok_ tText tFileError (str "old"))) <|
  and_ (eq (tRes tText tFileError) (io (.readText false) [v "files", str "dir/item"])
    (ok_ tText tFileError (str "kept"))) <|
  and_ (eq (tRes tText tFileError) (io (.readText false) [v "files", str "leafish/item"])
    (ok_ tText tFileError (str "sibling"))) <|
  and_ (eq (tRes tText tFileError) (io (.readText false) [v "files", str "gone"])
    (err_ tText tFileError (notFound "gone"))) <|
  and_ (io (.exists false) [v "files", str "leaf"]) <|
  and_ (io (.exists true) [v "read", str "dir"]) <|
  and_ (not_ (io (.exists false) [io (.within false) [v "files", str "leaf"], str "."])) <|
  and_ (not_ (io (.exists true) [io (.within true) [v "read", str "leaf"], str "child"])) <|
  and_ (eq (tRes tUnit tFileError) (io .writeText [v "files", str "new/item", str "made"])
    (err_ tUnit tFileError (notFound "new/item"))) <|
  and_ (eq (tRes tUnit tFileError) (io .appendText [v "files", str "new/item", str "!"])
    (err_ tUnit tFileError (notFound "new/item"))) <|
  and_ (not_ (io (.exists false) [v "files", str "new"])) <|
  and_ (eq (tRes tUnit tFileError) (io .makeFolder [v "files", str "new/deep"])
    (ok_ tUnit tFileError unitE)) <|
  and_ (eq (tRes tUnit tFileError) (io .makeFolder [v "files", str "new"])
    (ok_ tUnit tFileError unitE)) <|
  and_ (eq (tRes tUnit tFileError) (io .makeFolder [v "files", str "."])
    (ok_ tUnit tFileError unitE)) <|
  lets [("narrow", io (.within false) [v "files", str "new/deep"]),
    ("emptySnapshot", io (.list true) [io .readOnly [v "narrow"], str "."])] <|
  and_ (io (.exists false) [v "narrow", str "."]) <|
  and_ (eq (tRes tUnit tFileError) (io .makeFolder [v "narrow", str "./β/../z/empty"])
    (ok_ tUnit tFileError unitE)) <|
  and_ (eq (tRes tUnit tFileError) (io .makeFolder [v "narrow", str "../outside"])
    (err_ tUnit tFileError (notAllowed "../outside"))) <|
  and_ (not_ (io (.exists false) [v "files", str "new/outside"])) <|
  and_ (eq (tRes (tList tText) tFileError) (v "emptySnapshot")
    (ok_ (tList tText) tFileError (listOf tText []))) <|
  and_ (eq (tRes (tList tText) tFileError) (io (.list true) [v "read", str "new/deep"])
    (ok_ (tList tText) tFileError (listOf tText [str "z"]))) <|
  and_ (eq (tRes tUnit tFileError) (io .writeText [v "files", str "new/item", str "made"])
    (ok_ tUnit tFileError unitE)) <|
  and_ (eq (tRes tUnit tFileError) (io .appendText [v "files", str "new/item", str "!"])
    (ok_ tUnit tFileError unitE)) <|
  and_ (eq (tRes tText tFileError) (io (.readText true) [v "read", str "new/item"])
    (ok_ tText tFileError (str "made!"))) <|
  and_ (eq (tRes tUnit tFileError) (io .remove [v "files", str "new/item"])
    (ok_ tUnit tFileError unitE)) <|
  and_ (io (.exists false) [v "files", str "new"]) <|
  and_ (eq (tRes (tList .entry) tFileError)
    (io (.entries true) [io (.within true) [v "read", str "new/deep/z/empty"], str "."])
    (ok_ (tList .entry) tFileError (listOf .entry []))) <|
  eq (tRes (tList .entry) tFileError) (io (.entries true) [v "read", str "new"])
    (ok_ (tList .entry) tFileError (listOf .entry [entryE "deep" "Folder"]))

def kindFiles : List (String × String) :=
  [("leaf", "old"), ("dir/item", "kept"), ("leafish/item", "sibling")]

def fakeFileKindChecks : Expr :=
  lets [("files", io .filesInMemory [mapOf tText tText
    (kindFiles.map fun (p, text) => (str p, str text))]),
    ("otherFiles", io .filesInMemory [mapOf tText tText []])] <|
  and_ fileKindChecks (not_ (io (.exists false) [v "otherFiles", str "new"]))

#guard passes program fileKindChecks { files := kindFiles, folders := ["dir", "leafish"] } [("files", .files)]
#guard passes program fakeFileKindChecks
#guard ([
  ([("a", "file"), ("a/b", "child")], "a"),
  ([("a/b", "child"), ("a", "file")], "a"),
  ([("β/deep/n", "child"), ("β/deep", "file")], "β/deep"),
  ([("z/n", "child"), ("a", "file"), ("z", "file"), ("a/n", "child")], "a")] : List (List (String × String) × String)).all fun (files, path) =>
    stops program (io .filesInMemory [mapOf tText tText (files.map fun (p, text) => (str p, str text))])
      (tCap .files) (.fileFolderConflict path)

#guard (["/a", "", ".", "..", "./a", "../a", "a/", "a//b", "a/./b", "a/../b",
  "a/.", "a/..", "a\x00/../b"]).all fun path =>
    stops program (io .filesInMemory [mapOf tText tText [(str path, str "file")]])
      (tCap .files) (.filePathInvalid path)
#guard ([("a", "a/", "a/"), ("a//b", "/bad", "a//b"), ("valid", "../bad", "../bad")] :
  List (String × String × String)).all fun (first, second, expected) =>
    stops program (io .filesInMemory [mapOf tText tText [(str first, str second), (str second, str first)]])
      (tCap .files) (.filePathInvalid expected)

def fakeFileKeyChecks : Expr :=
  let keys := ["leaf", ".hidden", "...", "..name", "a/.b", "a/..b", "β/é", "a\x00b"]
  let files := mapOf tText tText (keys.map fun path => (str path, str "kept"))
  lets [("files", io .filesInMemory [files])] <|
    keys.foldr (fun path rest => and_
      (eq (tRes tText tFileError) (io (.readText false) [v "files", str path])
        (ok_ tText tFileError (str "kept"))) rest) tt

def libraryChecks : Expr :=
  and_ numberChecks (and_ randomChecks (and_ entryChecks (and_ fakeFileKindChecks fakeFileKeyChecks)))
def libraryProgram : Program :=
  { program with funs := program.funs ++
    [{ (fn "library" [] tUnit (assertThat libraryChecks (str "library check failed"))) with kind := .test }] ++
    Text.unicodeOps.map (fun (name, op) => fn ("Unicode16." ++ name) [("text", tText)] op.sig.2
      (prim (.unicode16 op) [v "text"])) }

#guard wellTyped? libraryProgram
#guard passes libraryProgram libraryChecks
#guard [UnicodeOp.graphemes, .displayWidth, .normalizeNfc, .lowercase, .uppercase].all fun op =>
  typedAs? program [] (prim (.unicode16 op) [str "éß👩‍💻"]) op.sig.2 &&
  !typedAs? program [] (prim (.unicode16 op) [int 0]) op.sig.2 &&
  stops program (prim (.unicode16 op) [str "éß👩‍💻"]) op.sig.2
    (.outside "Unicode16 is a trusted primitive unavailable in mocore")

-- Real-state dispatch, empty root, inaccessible/narrowed roots and persistent folders.
#guard passes program
  (eq (tRes (tList .entry) tFileError) (io (.entries false) [v "files", str "."])
    (ok_ (tList .entry) tFileError (listOf .entry [entryE "b" "File", entryE "é" "Folder"])))
  { files := [("é/x", "text"), ("b", "text")], folders := ["é"] } [("files", .files)]
#guard passes program
  (eq (tRes (tList .entry) tFileError) (io (.entries true) [v "read", str "."])
    (ok_ (tList .entry) tFileError (listOf .entry []))) {} [("read", .readFiles)]
#guard passes program
  (lets [("f", io .filesInMemory [mapOf tText tText [(str "a/x", str "x")]]),
    ("r", io .readOnly [v "f"])] <|
    and_ (eq (tRes (tList .entry) tFileError)
      (io (.entries true) [io (.within true) [v "r", str ".."], str "."])
      (err_ (tList .entry) tFileError (notAllowed "."))) <|
    seq (io .remove [v "f", str "a/x"]) <|
    and_ (eq (tRes (tList .entry) tFileError) (io (.entries true) [v "r", str "a"])
      (ok_ (tList .entry) tFileError (listOf .entry []))) <|
    eq (tRes (tList .entry) tFileError) (io (.entries false) [v "f", str "."])
      (ok_ (tList .entry) tFileError (listOf .entry [entryE "a" "Folder"])))
#guard stops program (io .randomInt [v "rng", int 3, int 3]) tInt
  (.outside "Random.int needs low below high") {} [("rng", .random)]
#guard stops program (io .randomInt [v "rng", int (-1), int 1]) tInt
  (.outside "Random has an all-zero state") { random := ⟨0, 0, 0, 0⟩ } [("rng", .random)]

end MoCore.Builtins
