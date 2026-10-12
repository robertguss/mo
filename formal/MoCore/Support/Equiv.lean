/-
  Mo Core support: structural equality and the built-in orders.

  `Equal` can't be customized in Mo (reference 9.2): values are equal when
  their fields are, maps compare their entries in any order, and floats follow
  IEEE rules. So one structural equality serves every type with `Equal`, and
  `Prim.eq` is it. Functions and capabilities have no `Equal`; they compare
  unequal here, and well-typed lowering never compares them.
-/
import MoCore.Support.Text
import MoCore.Support.Bytes

namespace MoCore

mutual
/-- Structural equality of values (Mo's `==`). -/
def Value.equiv : Value → Value → Bool
  | .int a, .int b | .bigInt a, .bigInt b | .byte a, .byte b | .decimal a, .decimal b
  | .time a, .time b | .duration a, .duration b => a == b
  | .float a, .float b => Float.ofBits a == Float.ofBits b
  | .bool a, .bool b => a == b
  | .char a, .char b => a == b
  | .text a, .text b => a == b
  | .bytes a, .bytes b => a == b
  | .unit, .unit => true
  | .tuple as, .tuple bs | .list as, .list bs => Value.equivList as bs
  | .con c as, .con d bs => c == d && Value.equivList as bs
  | .map es, .map fs => es.length == fs.length && Value.equivEntries es fs
  | _, _ => false
termination_by a _ => (sizeOf a, 0)
/-- Pointwise equality of two lists. -/
def Value.equivList : List Value → List Value → Bool
  | [], [] => true
  | a :: as, b :: bs => Value.equiv a b && Value.equivList as bs
  | _, _ => false
termination_by as _ => (sizeOf as, 0)
/-- Every entry of the first map has an equal entry in the second. -/
def Value.equivEntries : List (Value × Value) → List (Value × Value) → Bool
  | [], _ => true
  | (k, v) :: es, fs => Value.findEntry k v fs && Value.equivEntries es fs
termination_by es _ => (sizeOf es, 0)
/-- Whether a map's entries hold an entry equal to `(k, v)`. -/
def Value.findEntry : Value → Value → List (Value × Value) → Bool
  | _, _, [] => false
  | k, v, (k', v') :: fs => (Value.equiv k k' && Value.equiv v v') || Value.findEntry k v fs
termination_by k v fs => (sizeOf k + sizeOf v + 1, fs.length)
end

instance : BEq Value := ⟨Value.equiv⟩

/-- The built-in order of two values of a base type, or `none` when they are
not values of that type. Numbers (bytes too), times and durations by size,
`false` before `true`, characters by code, texts by character code, `Bytes`
byte by byte. Floats are left to `floatCmp`, because "not a number" is
unordered. -/
def baseOrder : Base → Value → Value → Option Ordering
  | .int, .int a, .int b | .bigInt, .bigInt a, .bigInt b | .byte, .byte a, .byte b
  | .decimal, .decimal a, .decimal b | .time, .time a, .time b
  | .duration, .duration a, .duration b => some (compare a b)
  | .bool, .bool a, .bool b => some (compare a.toNat b.toNat)
  | .char, .char a, .char b => some (compare a.toNat b.toNat)
  | .text, .text a, .text b =>
    some (if textLt a b then .lt else if textLt b a then .gt else .eq)
  | .bytes, .bytes a, .bytes b =>
    some (if bytesLt a b then .lt else if bytesLt b a then .gt else .eq)
  | _, _, _ => none

/-- Whether an ordering satisfies a comparison. -/
def CmpOp.holds : CmpOp → Ordering → Bool
  | .lt, o => o == .lt
  | .le, o => o != .gt
  | .gt, o => o == .gt
  | .ge, o => o != .lt

/-- IEEE comparison of two floats: false whenever either is not a number. -/
def floatCmp (op : CmpOp) (a b : Float) : Bool :=
  match op with
  | .lt => a < b
  | .le => a ≤ b
  | .gt => b < a
  | .ge => b ≤ a

end MoCore
