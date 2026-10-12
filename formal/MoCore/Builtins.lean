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
def program : Program := { data := [], funs := sortBy tByte tByte }

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

end MoCore.Builtins
