/-
  Mo Core: every primitive respects its signature.

  `prims_safe : PrimsSafe P` discharges the hypothesis of `eval_safe` for every
  primitive of `Prim`, in every program: given arguments of its argument
  types, a primitive returns a value of its result type or stops. In
  particular every `Int`, `Byte`, `Decimal`, `Duration` and `Time` it returns
  is in range: overflow stops, it never wraps.

  The proofs go group by group (`IntOp`, `DecOp`, `TextOp`, ...). Each one
  inverts the typing of the arguments (`invert_args`), unfolds the primitive,
  and closes the goal with the typing lemmas of the result helpers below.
-/
import MoCore.Safety

namespace MoCore

variable {P : Program}

/-! ## Inverting value typing -/

theorem valTy_int_iff : ValTy P v .int ↔ ∃ n, v = .int n ∧ InInt64 n := by
  constructor
  · intro h; cases h with
    | int hn => exact ⟨_, rfl, hn⟩
    | con hcs => simp [ctorsOf] at hcs
  · rintro ⟨n, rfl, hn⟩; exact .int hn

theorem valTy_bigInt_iff : ValTy P v .bigInt ↔ ∃ n, v = .bigInt n := by
  constructor
  · intro h; cases h with
    | bigInt => exact ⟨_, rfl⟩
    | con hcs => simp [ctorsOf] at hcs
  · rintro ⟨n, rfl⟩; exact .bigInt

theorem valTy_byte_iff : ValTy P v .byte ↔ ∃ n, v = .byte n ∧ InByte n := by
  constructor
  · intro h; cases h with
    | byte hn => exact ⟨_, rfl, hn⟩
    | con hcs => simp [ctorsOf] at hcs
  · rintro ⟨n, rfl, hn⟩; exact .byte hn

theorem valTy_decimal_iff : ValTy P v .decimal ↔ ∃ n, v = .decimal n ∧ InDecimal n := by
  constructor
  · intro h; cases h with
    | decimal hn => exact ⟨_, rfl, hn⟩
    | con hcs => simp [ctorsOf] at hcs
  · rintro ⟨n, rfl, hn⟩; exact .decimal hn

theorem valTy_float_iff : ValTy P v .float ↔ ∃ b, v = .float b := by
  constructor
  · intro h; cases h with
    | float => exact ⟨_, rfl⟩
    | con hcs => simp [ctorsOf] at hcs
  · rintro ⟨b, rfl⟩; exact .float

theorem valTy_bool_iff : ValTy P v .bool ↔ ∃ b, v = .bool b := by
  constructor
  · intro h; cases h with
    | bool => exact ⟨_, rfl⟩
    | con hcs => simp [ctorsOf] at hcs
  · rintro ⟨b, rfl⟩; exact .bool

theorem valTy_char_iff : ValTy P v .char ↔ ∃ c, v = .char c := by
  constructor
  · intro h; cases h with
    | char => exact ⟨_, rfl⟩
    | con hcs => simp [ctorsOf] at hcs
  · rintro ⟨c, rfl⟩; exact .char

theorem valTy_text_iff : ValTy P v .text ↔ ∃ s, v = .text s := by
  constructor
  · intro h; cases h with
    | text => exact ⟨_, rfl⟩
    | con hcs => simp [ctorsOf] at hcs
  · rintro ⟨s, rfl⟩; exact .text

theorem valTy_bytes_iff : ValTy P v .bytes ↔ ∃ bs, v = .bytes bs ∧ ∀ b ∈ bs, InByte b := by
  constructor
  · intro h; cases h with
    | bytes h => exact ⟨_, rfl, h⟩
    | con hcs => simp [ctorsOf] at hcs
  · rintro ⟨bs, rfl, h⟩; exact .bytes h

theorem valTy_unit_iff : ValTy P v .unit ↔ v = .unit := by
  constructor
  · intro h; cases h with
    | unit => rfl
    | con hcs => simp [ctorsOf] at hcs
  · rintro rfl; exact .unit

theorem valTy_range_iff : ValTy P v .range ↔ ∃ a b, v = .range a b ∧ InInt64 a ∧ InInt64 b := by
  constructor
  · intro h; cases h with
    | range ha hb => exact ⟨_, _, rfl, ha, hb⟩
    | con hcs => simp [ctorsOf] at hcs
  · rintro ⟨a, b, rfl, ha, hb⟩; exact .range ha hb

theorem valTy_time_iff : ValTy P v .time ↔ ∃ t, v = .time t ∧ InTime t := by
  constructor
  · intro h; cases h with
    | time ht => exact ⟨_, rfl, ht⟩
    | con hcs => simp [ctorsOf] at hcs
  · rintro ⟨t, rfl, ht⟩; exact .time ht

theorem valTy_duration_iff : ValTy P v .duration ↔ ∃ d, v = .duration d ∧ InInt64 d := by
  constructor
  · intro h; cases h with
    | duration hd => exact ⟨_, rfl, hd⟩
    | con hcs => simp [ctorsOf] at hcs
  · rintro ⟨d, rfl, hd⟩; exact .duration hd

theorem valTy_list_iff : ValTy P v (.list t) ↔ ∃ vs, v = .list vs ∧ ∀ x ∈ vs, ValTy P x t := by
  constructor
  · intro h; cases h with
    | list h => exact ⟨_, rfl, h⟩
    | con hcs => simp [ctorsOf] at hcs
  · rintro ⟨vs, rfl, h⟩; exact .list h

theorem valTy_map_iff : ValTy P v (.map k t) ↔
    ∃ es, v = .map es ∧ (∀ e ∈ es, ValTy P e.1 k) ∧ (∀ e ∈ es, ValTy P e.2 t) := by
  constructor
  · intro h; cases h with
    | map hk hv => exact ⟨_, rfl, hk, hv⟩
    | con hcs => simp [ctorsOf] at hcs
  · rintro ⟨es, rfl, hk, hv⟩; exact .map hk hv

theorem valTy_set_iff : ValTy P v (.set t) ↔
    ∃ vs, v = .set vs ∧ (∀ x ∈ vs, ValTy P x t) ∧ Distinct vs := by
  constructor
  · intro h; cases h with
    | set h hd => exact ⟨_, rfl, h, hd⟩
    | con hcs => simp [ctorsOf] at hcs
  · rintro ⟨vs, rfl, h, hd⟩; exact .set h hd

theorem valTy_tuple_iff : ValTy P v (.tuple ts) ↔ ∃ vs, v = .tuple vs ∧ ValsTy P vs ts := by
  constructor
  · intro h; cases h with
    | tuple h => exact ⟨_, rfl, h⟩
    | con hcs => simp [ctorsOf] at hcs
  · rintro ⟨vs, rfl, h⟩; exact .tuple h

theorem valTy_cap_iff : ValTy P v (.cap c) ↔ ∃ r, v = .cap c r := by
  constructor
  · intro h; cases h with
    | cap => exact ⟨_, rfl⟩
    | con hcs => simp [ctorsOf] at hcs
  · rintro ⟨r, rfl⟩; exact .cap

theorem valsTy_nil_iff : ValsTy P vs [] ↔ vs = [] := by
  constructor
  · intro h; cases h; rfl
  · rintro rfl; exact .nil

theorem valsTy_cons_iff : ValsTy P vs (t :: ts) ↔ ∃ x xs, vs = x :: xs ∧ ValTy P x t ∧ ValsTy P xs ts := by
  constructor
  · intro h; cases h with
    | cons hx hxs => exact ⟨_, _, rfl, hx, hxs⟩
  · rintro ⟨x, xs, rfl, hx, hxs⟩; exact .cons hx hxs

/-! ## Typing the results -/

theorem ValTy.someV {x : Value} {t : Ty} (h : ValTy P x t) : ValTy P (someV x) (.option t) :=
  .con rfl rfl (.cons h .nil)

theorem ValTy.noneV {t : Ty} : ValTy P noneV (.option t) := .con rfl rfl .nil

theorem ValTy.okV {x : Value} {t e : Ty} (h : ValTy P x t) : ValTy P (okV x) (.result t e) :=
  .con rfl rfl (.cons h .nil)

theorem ValTy.errorV {x : Value} {t e : Ty} (h : ValTy P x e) : ValTy P (errorV x) (.result t e) :=
  .con rfl rfl (.cons h .nil)

theorem ValTy.notFound {path : String} : ValTy P (fileErrorV "NotFound" path) .fileError :=
  .con rfl rfl (.cons .text .nil)

theorem ValTy.notAllowed {path : String} : ValTy P (fileErrorV "NotAllowed" path) .fileError :=
  .con rfl rfl (.cons .text .nil)

theorem ValTy.optOf {o : Option Value} {t : Ty} (h : ∀ x, o = some x → ValTy P x t) :
    ValTy P (optOf o) (.option t) := by
  cases o with
  | none => exact .noneV
  | some x => exact .someV (h x rfl)

theorem ValTy.optInt {n : Int} : ValTy P (optInt n) (.option .int) := by
  unfold MoCore.optInt; split
  · exact .someV (.int ‹_›)
  · exact .noneV

theorem ValTy.optByte {n : Int} : ValTy P (optByte n) (.option .byte) := by
  unfold MoCore.optByte; split
  · exact .someV (.byte ‹_›)
  · exact .noneV

theorem ValTy.optDec {n : Int} : ValTy P (optDec n) (.option .decimal) := by
  unfold MoCore.optDec; split
  · exact .someV (.decimal ‹_›)
  · exact .noneV

theorem ValTy.optTime {n : Int} : ValTy P (optTime n) (.option .time) := by
  unfold MoCore.optTime; split
  · exact .someV (.time ‹_›)
  · exact .noneV

theorem ValTy.optIntOf {o : Option Int} : ValTy P (optIntOf o) (.option .int) := by
  cases o
  · exact .noneV
  · exact .optInt

theorem ValTy.optDecOf {o : Option Int} : ValTy P (optDecOf o) (.option .decimal) := by
  cases o
  · exact .noneV
  · exact .optDec

theorem ValTy.optTimeOf {o : Option Int} : ValTy P (optTimeOf o) (.option .time) := by
  cases o
  · exact .noneV
  · exact .optTime

theorem ValTy.optText {o : Option String} : ValTy P (optText o) (.option .text) := by
  cases o
  · exact .noneV
  · exact .someV .text

theorem ValTy.optChar {o : Option Char} : ValTy P (optChar o) (.option .char) := by
  cases o
  · exact .noneV
  · exact .someV .char

theorem ValTy.decToIntV {a : Int} : ValTy P (decToIntV a) (.option .int) := by
  unfold MoCore.decToIntV; split
  · exact .optInt
  · exact .noneV

theorem ValTy.texts {xs : List String} : ValTy P (texts xs) (.list .text) :=
  .list fun x hx => by
    simp only [List.mem_map] at hx
    obtain ⟨s, _, rfl⟩ := hx
    exact .text

theorem okInt_safe {n : Int} {w : World} {Δ : JCtx} : OutcomeOk P Δ .int (okInt n w) := by
  unfold okInt; split
  · exact .int ‹_›
  · trivial

theorem okDec_safe {n : Int} {w : World} {Δ : JCtx} : OutcomeOk P Δ .decimal (okDec n w) := by
  unfold okDec; split
  · exact .decimal ‹_›
  · trivial

theorem okDur_safe {n : Int} {w : World} {Δ : JCtx} : OutcomeOk P Δ .duration (okDur n w) := by
  unfold okDur; split
  · exact .duration ‹_›
  · trivial

theorem okTime_safe {n : Int} {w : World} {Δ : JCtx} : OutcomeOk P Δ .time (okTime n w) := by
  unfold okTime; split
  · exact .time ‹_›
  · trivial

theorem outside_safe {what : String} {w : World} {Δ : JCtx} {τ : Ty} :
    OutcomeOk P Δ τ (outside what w) := trivial

theorem outcomeOk_ite {c : Prop} [Decidable c] {a b : Outcome} {Δ : JCtx} {τ : Ty} :
    OutcomeOk P Δ τ (if c then a else b) ↔ (c → OutcomeOk P Δ τ a) ∧ (¬c → OutcomeOk P Δ τ b) := by
  by_cases h : c <;> simp [h]

theorem valTy_ite {c : Prop} [Decidable c] {a b : Value} {τ : Ty} :
    ValTy P (if c then a else b) τ ↔ (c → ValTy P a τ) ∧ (¬c → ValTy P b τ) := by
  by_cases h : c <;> simp [h]

/-! ## Inverting a primitive's arguments -/

/-- Turns the typing of a primitive's arguments into equations for the
argument values (substituted away) and their range facts. -/
macro "invert_args" : tactic => `(tactic| (
  intro vs w h
  revert h
  simp only [Prim.sig, IntOp.sig, BigOp.sig, ByteOp.sig, DecOp.sig, FloatOp.sig, CharOp.sig,
    TextOp.sig, BytesOp.sig, RangeOp.sig, TimeOp.sig, ListOp.sig, MapOp.sig, SetOp.sig,
    IoOp.sig, Base.ty, filesCap, valsTy_cons_iff, valsTy_nil_iff, valTy_int_iff,
    valTy_bigInt_iff, valTy_byte_iff, valTy_decimal_iff, valTy_float_iff, valTy_bool_iff,
    valTy_char_iff, valTy_text_iff, valTy_bytes_iff, valTy_range_iff, valTy_time_iff,
    valTy_duration_iff, valTy_list_iff, valTy_map_iff, valTy_set_iff, valTy_cap_iff,
    forall_exists_index, and_imp]
  intros
  subst_vars))

/-! ## Numbers -/

theorem intOp_safe (op : IntOp) : PrimSafe P (.int op) := by
  cases op <;> invert_args <;>
    simp only [Prim.eval, evalIntOp, outcomeOk_ite, outcomeOk_ok, outcomeOk_stopped] <;>
    first
    | exact okInt_safe
    | exact okDec_safe
    | exact okDur_safe
    | exact .optInt
    | exact .optChar
    | exact .optByte
    | exact .text
    | exact .bigInt
    | exact .float
    | exact ⟨fun _ => trivial, fun _ => okInt_safe⟩
    | skip

theorem bigOp_safe (op : BigOp) : PrimSafe P (.big op) := by
  cases op <;> invert_args <;>
    simp only [Prim.eval, evalBigOp, outcomeOk_ite, outcomeOk_ok, outcomeOk_stopped] <;>
    first
    | exact .bigInt
    | exact .optInt
    | exact .text
    | exact ⟨fun _ => trivial, fun _ => .bigInt⟩

theorem byteOp_safe (op : ByteOp) : PrimSafe P (.byte op) := by
  cases op <;> invert_args <;> simp only [Prim.eval, evalByteOp, outcomeOk_ok] <;>
    first
    | exact okInt_safe
    | exact .text

theorem decOp_safe (op : DecOp) : PrimSafe P (.dec op) := by
  cases op <;> invert_args <;>
    simp only [Prim.eval, evalDecOp, outcomeOk_ite, outcomeOk_ok, outcomeOk_stopped] <;>
    first
    | exact okDec_safe
    | exact .decToIntV
    | exact .text
    | exact .float
    | exact ⟨fun _ => trivial, fun _ => okDec_safe⟩
    | exact ⟨fun _ => okDec_safe, fun _ => outside_safe⟩
    | exact ⟨fun _ => .text, fun _ => outside_safe⟩

theorem floatOp_safe (op : FloatOp) : PrimSafe P (.float op) := by
  cases op <;> invert_args <;> simp only [Prim.eval, evalFloatOp, outcomeOk_ok] <;>
    first
    | exact .float
    | exact .bool
    | exact .text

theorem not_safe : PrimSafe P .not := by
  invert_args
  exact .bool

theorem cmp_safe (b : Base) (op : CmpOp) : PrimSafe P (.cmp b op) := by
  cases b <;> invert_args <;> exact .bool

theorem charOp_safe (op : CharOp) : PrimSafe P (.char op) := by
  cases op <;> invert_args <;> simp only [Prim.eval, evalCharOp, outcomeOk_ok] <;>
    first
    | exact okInt_safe
    | exact .bool
    | exact .char
    | exact .text

/-- A list of `Char` values has its characters. -/
theorem charsOf_some : ∀ {cs : List Value}, (∀ x ∈ cs, ∃ c, x = .char c) → ∃ l, charsOf cs = some l
  | [], _ => ⟨[], rfl⟩
  | c :: cs, h => by
    obtain ⟨ch, rfl⟩ := h c (by simp)
    obtain ⟨l, hl⟩ := charsOf_some (cs := cs) fun x hx => h x (by simp [hx])
    exact ⟨ch :: l, by simp [charsOf, hl]⟩

/-- A list of `Text` values has its texts. -/
theorem textsOf_some : ∀ {xs : List Value}, (∀ x ∈ xs, ∃ s, x = .text s) → ∃ l, textsOf xs = some l
  | [], _ => ⟨[], rfl⟩
  | x :: xs, h => by
    obtain ⟨s, rfl⟩ := h x (by simp)
    obtain ⟨l, hl⟩ := textsOf_some (xs := xs) fun y hy => h y (by simp [hy])
    exact ⟨s :: l, by simp [textsOf, hl]⟩

/-- A character's code is below `0x110000`. -/
theorem char_toNat_lt (c : Char) : c.toNat < 0x110000 := by
  have := c.valid
  simp only [UInt32.isValidChar, Nat.isValidChar] at this
  simp only [Char.toNat]
  omega

/-- Every byte of a text's UTF-8 encoding is from 0 to 255. -/
theorem textBytes_inByte (s : String) : ∀ b ∈ textBytes s, InByte b := by
  intro b hb
  simp only [textBytes, List.mem_flatMap] at hb
  obtain ⟨c, _, hb⟩ := hb
  have := char_toNat_lt c
  unfold utf8EncodeChar at hb
  dsimp only at hb
  unfold InByte
  split at hb <;> (try split at hb) <;> (try split at hb) <;>
    simp only [List.mem_cons, List.not_mem_nil, or_false] at hb <;> omega

theorem ValTy.textBytes {s : String} : ValTy P (.bytes (textBytes s)) .bytes :=
  .bytes (textBytes_inByte s)

theorem textOp_safe (op : TextOp) : PrimSafe P (.text op) := by
  cases op
  case fromChars =>
    invert_args
    obtain ⟨l, hl⟩ := charsOf_some ‹_›
    simp only [Prim.eval, evalTextOp, hl, outcomeOk_ok]
    exact .text
  case join =>
    invert_args
    obtain ⟨l, hl⟩ := textsOf_some ‹_›
    simp only [Prim.eval, evalTextOp, hl, outcomeOk_ok]
    exact .text
  case chars =>
    invert_args
    simp only [Prim.eval, evalTextOp, outcomeOk_ok]
    refine .list fun x hx => ?_
    simp only [List.mem_map] at hx
    obtain ⟨c, _, rfl⟩ := hx
    exact .char
  all_goals
    invert_args
    simp only [Prim.eval, evalTextOp, outcomeOk_ite, outcomeOk_ok]
    first
    | exact .text
    | exact okInt_safe
    | exact .bool
    | exact .texts
    | exact .optText
    | exact .optIntOf
    | exact .optDecOf
    | exact .textBytes
    | exact ⟨fun _ => trivial, fun _ => .texts⟩
    | exact ⟨fun _ => trivial, fun _ => .text⟩

theorem bytesGet_ty {bs : List Int} {i : Int} (h : ∀ b ∈ bs, InByte b) :
    ValTy P (bytesGet bs i) (.option .byte) := by
  unfold bytesGet; split
  · refine .optOf fun x hx => ?_
    obtain ⟨b, hb, rfl⟩ := Option.map_eq_some_iff.mp hx
    exact .byte (h b (List.mem_of_getElem? hb))
  · exact .noneV

theorem bytesOp_safe (op : BytesOp) : PrimSafe P (.bytes op) := by
  cases op <;> invert_args <;> simp only [Prim.eval, evalBytesOp, outcomeOk_ok]
  case size => exact okInt_safe
  case get => exact bytesGet_ty ‹_›
  case decodeUtf8 => exact .optText

/-- The whole numbers of a range between two `Int`s are `Int`s. -/
theorem intRange_ty {a b : Int} (ha : InInt64 a) (hb : InInt64 b) :
    ∀ x ∈ intRange a b, ValTy P x .int := by
  intro x hx
  simp only [intRange, List.mem_map, List.mem_range] at hx
  obtain ⟨i, hi, rfl⟩ := hx
  apply ValTy.int
  unfold InInt64 at *
  omega

theorem rangeOp_safe (op : RangeOp) : PrimSafe P (.range op) := by
  cases op <;> invert_args <;> simp only [Prim.eval, evalRangeOp, outcomeOk_ok]
  case make => exact .range ‹_› ‹_›
  case start => exact .int ‹_›
  case «end» => exact .int ‹_›
  case toList => exact .list (intRange_ty ‹_› ‹_›)
  case contains => exact .bool
  case toText => exact .text

theorem timeOp_safe (op : TimeOp) : PrimSafe P (.time op) := by
  cases op <;> invert_args <;> simp only [Prim.eval, evalTimeOp, outcomeOk_ok] <;>
    first
    | exact okDur_safe
    | exact okTime_safe
    | exact okInt_safe
    | exact okDec_safe
    | exact .optTime
    | exact .optTimeOf
    | exact .text

/-! ## Lists -/

theorem listGet_ty {xs : List Value} {t : Ty} {i : Int} (h : ∀ x ∈ xs, ValTy P x t) :
    ValTy P (listGet xs i) (.option t) := by
  unfold listGet; split
  · exact .optOf fun x hx => h x (List.mem_of_getElem? hx)
  · exact .noneV

theorem unconsV_ty {xs : List Value} {t : Ty} (h : ∀ x ∈ xs, ValTy P x t) :
    ValTy P (unconsV xs) (.option (.tuple [t, .list t])) := by
  cases xs with
  | nil => exact .noneV
  | cons x rest =>
    exact .someV (.tuple (.cons (h x (by simp)) (.cons (.list fun y hy => h y (by simp [hy])) .nil)))

theorem enumerateVals_mem : ∀ {k : Nat} {xs : List Value} {y : Value}, y ∈ enumerateVals k xs →
    ∃ (i : Nat) (x : Value), y = .tuple [.int i, x] ∧ i < k + xs.length ∧ x ∈ xs
  | _, [], _, h => by simp [enumerateVals] at h
  | k, x :: xs, y, h => by
    simp only [enumerateVals, List.mem_cons] at h
    rcases h with rfl | h
    · exact ⟨k, x, rfl, by simp, by simp⟩
    · obtain ⟨i, x', rfl, hi, hx'⟩ := enumerateVals_mem h
      exact ⟨i, x', rfl, by simp at hi ⊢; omega, by simp [hx']⟩

/-- Adding an element to a set's elements keeps them typed, and none equal to
another. -/
theorem setAdd_ok {xs : List Value} {x : Value} {t : Ty} (hxs : ∀ y ∈ xs, ValTy P y t)
    (hd : Distinct xs) (hx : ValTy P x t) :
    (∀ y ∈ setAdd xs x, ValTy P y t) ∧ Distinct (setAdd xs x) := by
  unfold setAdd
  split
  · exact ⟨hxs, hd⟩
  · rename_i hnew
    simp only [setHas, List.any_eq_true, not_exists, not_and, Bool.not_eq_true] at hnew
    refine ⟨fun y hy => ?_, List.pairwise_append.mpr ⟨hd, by simp, fun a ha b hb => ?_⟩⟩
    · simp only [List.mem_append, List.mem_singleton] at hy
      rcases hy with hy | rfl
      · exact hxs y hy
      · exact hx
    · simp only [List.mem_singleton] at hb
      subst hb
      exact hnew a ha

/-- Adding elements in turn, as `List.unique`, `List.to_set` and `union` do. -/
theorem foldl_setAdd_ok {t : Ty} : ∀ {ys xs : List Value}, (∀ y ∈ xs, ValTy P y t) →
    Distinct xs → (∀ y ∈ ys, ValTy P y t) →
    (∀ y ∈ ys.foldl setAdd xs, ValTy P y t) ∧ Distinct (ys.foldl setAdd xs)
  | [], _, hxs, hd, _ => ⟨hxs, hd⟩
  | y :: ys, _, hxs, hd, hys => by
    simp only [List.foldl_cons]
    obtain ⟨h1, h2⟩ := setAdd_ok hxs hd (hys y (by simp))
    exact foldl_setAdd_ok h1 h2 fun z hz => hys z (by simp [hz])

theorem uniqueVals_ok {xs : List Value} {t : Ty} (h : ∀ x ∈ xs, ValTy P x t) :
    (∀ y ∈ uniqueVals xs, ValTy P y t) ∧ Distinct (uniqueVals xs) :=
  foldl_setAdd_ok (by simp) List.Pairwise.nil h

theorem zipWith_mem : ∀ {xs ys : List Value} {y : Value},
    y ∈ List.zipWith (fun a b => Value.tuple [a, b]) xs ys → ∃ a ∈ xs, ∃ b ∈ ys, y = .tuple [a, b]
  | [], _, _, h => by simp at h
  | _ :: _, [], _, h => by simp at h
  | a :: xs, b :: ys, y, h => by
    simp only [List.zipWith_cons_cons, List.mem_cons] at h
    rcases h with rfl | h
    · exact ⟨a, by simp, b, by simp, rfl⟩
    · obtain ⟨a', ha', b', hb', rfl⟩ := zipWith_mem h
      exact ⟨a', by simp [ha'], b', by simp [hb'], rfl⟩

theorem listOp_safe (t : Ty) (op : ListOp) : PrimSafe P (.list t op) := by
  cases op
  case lit n =>
    intro vs w h
    simp only [Prim.sig, ListOp.sig] at h
    exact .list (ValsTy.of_replicate h)
  case enumerate =>
    invert_args
    simp only [Prim.eval, evalListOp, outcomeOk_ite, outcomeOk_ok, outcomeOk_stopped]
    refine ⟨fun hl => .list fun y hy => ?_, fun _ => trivial⟩
    obtain ⟨i, x, rfl, hi, hx⟩ := enumerateVals_mem hy
    refine .tuple (.cons (.int ?_) (.cons (‹∀ x ∈ _, ValTy P x t› x hx) .nil))
    unfold InInt64 int64Min at *
    omega
  case zip u =>
    invert_args
    simp only [Prim.eval, evalListOp, outcomeOk_ok]
    refine .list fun y hy => ?_
    obtain ⟨a, ha, b, hb, rfl⟩ := zipWith_mem hy
    exact .tuple (.cons (‹∀ x ∈ _, ValTy P x t› a ha) (.cons (‹∀ x ∈ _, ValTy P x u› b hb) .nil))
  all_goals
    invert_args
    simp only [Prim.eval, evalListOp, outcomeOk_ite, outcomeOk_ok]
  case size => exact okInt_safe
  case isEmpty => exact .bool
  case get => exact listGet_ty ‹_›
  case first => exact .optOf fun x hx => ‹∀ x ∈ _, ValTy P x t› x (List.mem_of_mem_head? hx)
  case last => exact .optOf fun x hx => ‹∀ x ∈ _, ValTy P x t› x (List.mem_of_getLast? hx)
  case append =>
    exact .list fun y hy => by
      simp only [List.mem_append, List.mem_singleton] at hy
      rcases hy with hy | rfl
      · exact ‹∀ x ∈ _, ValTy P x t› y hy
      · assumption
  case prepend =>
    exact .list fun y hy => by
      simp only [List.mem_cons] at hy
      rcases hy with rfl | hy
      · assumption
      · exact ‹∀ x ∈ _, ValTy P x t› y hy
  case concat =>
    rename_i xs h1 ys h2
    exact .list fun y hy => by
      simp only [List.mem_append] at hy
      rcases hy with hy | hy
      · exact h1 y hy
      · exact h2 y hy
  case reverse => exact .list fun y hy => ‹∀ x ∈ _, ValTy P x t› y (List.mem_reverse.mp hy)
  case take =>
    exact ⟨fun _ => trivial, fun _ => .list fun y hy => ‹∀ x ∈ _, ValTy P x t› y (List.mem_of_mem_take hy)⟩
  case drop =>
    exact ⟨fun _ => trivial, fun _ => .list fun y hy => ‹∀ x ∈ _, ValTy P x t› y (List.mem_of_mem_drop hy)⟩
  case set =>
    rename_i xs hxs i _ x hx
    refine valTy_ite.mpr ⟨fun _ => .someV (.list fun y hy => ?_), fun _ => .noneV⟩
    rcases List.mem_or_eq_of_mem_set hy with hy | rfl
    · exact hxs y hy
    · exact hx
  case removeAt =>
    refine valTy_ite.mpr ⟨fun _ => .someV (.list fun y hy => ?_), fun _ => .noneV⟩
    exact ‹∀ x ∈ _, ValTy P x t› y (List.mem_of_mem_eraseIdx hy)
  case uncons => exact unconsV_ty ‹_›
  case contains => exact .bool
  case unique => exact .list (uniqueVals_ok ‹_›).1

/-! ## Maps -/

theorem mapGet_mem : ∀ {k : Value} {es : List (Value × Value)} {v : Value},
    mapGet k es = some v → ∃ e ∈ es, e.2 = v
  | _, [], _, h => by simp [mapGet] at h
  | k, (k', v') :: es, v, h => by
    simp only [mapGet] at h
    split at h
    · simp only [Option.some.injEq] at h
      exact ⟨(k', v'), by simp, h⟩
    · obtain ⟨e, he, rfl⟩ := mapGet_mem h
      exact ⟨e, by simp [he], rfl⟩

theorem mapSet_ty {es : List (Value × Value)} {k v : Value} {K V : Ty}
    (hk : ∀ e ∈ es, ValTy P e.1 K) (hv : ∀ e ∈ es, ValTy P e.2 V)
    (hk' : ValTy P k K) (hv' : ValTy P v V) :
    (∀ e ∈ mapSet es k v, ValTy P e.1 K) ∧ (∀ e ∈ mapSet es k v, ValTy P e.2 V) := by
  unfold mapSet
  split
  · constructor
    · intro e he
      simp only [List.mem_map] at he
      obtain ⟨e', he', rfl⟩ := he
      split
      · exact hk e' he'
      · exact hk e' he'
    · intro e he
      simp only [List.mem_map] at he
      obtain ⟨e', he', rfl⟩ := he
      split
      · exact hv'
      · exact hv e' he'
  · constructor
    · intro e he
      simp only [List.mem_append, List.mem_singleton] at he
      rcases he with he | rfl
      · exact hk e he
      · exact hk'
    · intro e he
      simp only [List.mem_append, List.mem_singleton] at he
      rcases he with he | rfl
      · exact hv e he
      · exact hv'

theorem pairsOf_some : ∀ {ps : List Value} {K V : Ty}, (∀ x ∈ ps, ValTy P x (.tuple [K, V])) →
    ∃ kvs, pairsOf ps = some kvs ∧ (∀ e ∈ kvs, ValTy P e.1 K) ∧ (∀ e ∈ kvs, ValTy P e.2 V)
  | [], _, _, _ => ⟨[], rfl, by simp, by simp⟩
  | p :: ps, K, V, h => by
    obtain ⟨vs, rfl, hvs⟩ := valTy_tuple_iff.mp (h p (by simp))
    obtain ⟨a, _, rfl, ha, hrest⟩ := valsTy_cons_iff.mp hvs
    obtain ⟨b, _, rfl, hb, hnil⟩ := valsTy_cons_iff.mp hrest
    obtain rfl := valsTy_nil_iff.mp hnil
    obtain ⟨kvs, hkvs, hK, hV⟩ := pairsOf_some (ps := ps) fun x hx => h x (by simp [hx])
    refine ⟨(a, b) :: kvs, by simp [pairsOf, hkvs], ?_, ?_⟩
    · intro e he
      simp only [List.mem_cons] at he
      rcases he with rfl | he
      · exact ha
      · exact hK e he
    · intro e he
      simp only [List.mem_cons] at he
      rcases he with rfl | he
      · exact hb
      · exact hV e he

theorem mapOfPairs_ty {K V : Ty} : ∀ {kvs : List (Value × Value)} {m : List (Value × Value)},
    (∀ e ∈ kvs, ValTy P e.1 K) → (∀ e ∈ kvs, ValTy P e.2 V) →
    (∀ e ∈ m, ValTy P e.1 K) → (∀ e ∈ m, ValTy P e.2 V) →
    (∀ e ∈ kvs.foldl (fun m kv => mapSet m kv.1 kv.2) m, ValTy P e.1 K) ∧
    (∀ e ∈ kvs.foldl (fun m kv => mapSet m kv.1 kv.2) m, ValTy P e.2 V)
  | [], _, _, _, h1, h2 => ⟨h1, h2⟩
  | kv :: kvs, _, hK, hV, h1, h2 => by
    simp only [List.foldl_cons]
    obtain ⟨h1', h2'⟩ := mapSet_ty h1 h2 (hK kv (by simp)) (hV kv (by simp))
    exact mapOfPairs_ty (fun e he => hK e (by simp [he])) (fun e he => hV e (by simp [he])) h1' h2'

/-- A map literal's arguments, keys and values in turn, are its entries. -/
theorem litEntries_some {K V : Ty} : ∀ {n : Nat} {vs : List Value}, ValsTy P vs (mapLitTys K V n) →
    ∃ es, litEntries vs = some es ∧ (∀ e ∈ es, ValTy P e.1 K) ∧ (∀ e ∈ es, ValTy P e.2 V)
  | 0, vs, h => by
    simp only [mapLitTys, valsTy_nil_iff] at h
    subst h
    exact ⟨[], rfl, by simp, by simp⟩
  | n + 1, vs, h => by
    simp only [mapLitTys] at h
    obtain ⟨a, rest, rfl, ha, hrest⟩ := valsTy_cons_iff.mp h
    obtain ⟨b, rest', rfl, hb, hrest'⟩ := valsTy_cons_iff.mp hrest
    obtain ⟨es, hes, hK, hV⟩ := litEntries_some hrest'
    refine ⟨(a, b) :: es, by simp [litEntries, hes], ?_, ?_⟩
    · intro e he
      simp only [List.mem_cons] at he
      rcases he with rfl | he
      · exact ha
      · exact hK e he
    · intro e he
      simp only [List.mem_cons] at he
      rcases he with rfl | he
      · exact hb
      · exact hV e he

theorem mapOp_safe (k t : Ty) (op : MapOp) : PrimSafe P (.map k t op) := by
  cases op
  case lit n =>
    intro vs w h
    simp only [Prim.sig, MapOp.sig] at h
    obtain ⟨es, hes, hK, hV⟩ := litEntries_some h
    simp only [Prim.eval, evalMapOp, hes]
    split
    · exact .map hK hV
    · trivial
  case ofList =>
    invert_args
    obtain ⟨kvs, hkvs, hK, hV⟩ := pairsOf_some ‹_›
    simp only [Prim.eval, evalMapOp, hkvs, outcomeOk_ok]
    obtain ⟨h1, h2⟩ := mapOfPairs_ty (m := []) hK hV (by simp) (by simp)
    exact .map h1 h2
  all_goals
    invert_args
    simp only [Prim.eval, evalMapOp, outcomeOk_ok]
  case empty => exact .map (by simp) (by simp)
  case size => exact okInt_safe
  case isEmpty => exact .bool
  case get =>
    exact .optOf fun x hx => by
      obtain ⟨e, he, rfl⟩ := mapGet_mem hx
      exact ‹∀ e ∈ _, ValTy P e.2 t› e he
  case has => exact .bool
  case set =>
    rename_i es hes1 hes2 _ hk _ hv
    obtain ⟨h1, h2⟩ := mapSet_ty hes1 hes2 hk hv
    exact .map h1 h2
  case remove =>
    exact .map (fun e he => ‹∀ e ∈ _, ValTy P e.1 k› e (List.mem_filter.mp he).1)
      (fun e he => ‹∀ e ∈ _, ValTy P e.2 t› e (List.mem_filter.mp he).1)
  case keys =>
    refine .list fun x hx => ?_
    simp only [List.mem_map] at hx
    obtain ⟨e, he, rfl⟩ := hx
    exact ‹∀ e ∈ _, ValTy P e.1 k› e he
  case values =>
    refine .list fun x hx => ?_
    simp only [List.mem_map] at hx
    obtain ⟨e, he, rfl⟩ := hx
    exact ‹∀ e ∈ _, ValTy P e.2 t› e he
  case entries =>
    refine .list fun x hx => ?_
    simp only [List.mem_map] at hx
    obtain ⟨e, he, rfl⟩ := hx
    exact .tuple (.cons (‹∀ e ∈ _, ValTy P e.1 k› e he) (.cons (‹∀ e ∈ _, ValTy P e.2 t› e he) .nil))

/-! ## Sets -/

/-- Some of a set's elements, as `remove`, `intersection` and `difference` keep
them: still typed, and none equal to another. -/
theorem setFilter_ty {xs : List Value} {t : Ty} (p : Value → Bool) (hxs : ∀ y ∈ xs, ValTy P y t)
    (hd : Distinct xs) : ValTy P (.set (xs.filter p)) (.set t) :=
  .set (fun y hy => hxs y (List.mem_filter.mp hy).1) (List.Pairwise.filter p hd)

theorem setOp_safe (t : Ty) (op : SetOp) : PrimSafe P (.set t op) := by
  cases op <;> invert_args <;> simp only [Prim.eval, evalSetOp, outcomeOk_ok]
  case empty => exact .set (by simp) List.Pairwise.nil
  case size => exact okInt_safe
  case isEmpty => exact .bool
  case has => exact .bool
  case add =>
    obtain ⟨h1, h2⟩ := setAdd_ok ‹_› ‹_› ‹_›
    exact .set h1 h2
  case remove => exact setFilter_ty _ ‹_› ‹_›
  case union =>
    rename_i xs hxs hd ys hys _
    obtain ⟨h1, h2⟩ := foldl_setAdd_ok hxs hd hys
    exact .set h1 h2
  case intersection => exact setFilter_ty _ ‹_› ‹_›
  case difference => exact setFilter_ty _ ‹_› ‹_›
  case toList => exact .list ‹_›
  case ofList =>
    obtain ⟨h1, h2⟩ := uniqueVals_ok ‹_›
    exact .set h1 h2

theorem eq_safe (t : Ty) : PrimSafe P (.eq t) := by
  invert_args
  exact .bool

/-! ## Capabilities -/

theorem ioOp_safe (op : IoOp) : PrimSafe P (.io op) := by
  cases op <;> invert_args <;>
    simp only [Prim.eval, evalIoOp, capRoot, filesKind, ↓reduceIte] <;>
    (repeat' split) <;>
    first
    | exact ValTy.unit
    | exact ValTy.texts
    | exact ValTy.optText
    | exact ValTy.someV ValTy.text
    | exact ValTy.noneV
    | exact ValTy.okV ValTy.text
    | exact ValTy.okV ValTy.textBytes
    | exact ValTy.okV ValTy.unit
    | exact ValTy.okV ValTy.texts
    | exact ValTy.errorV ValTy.notFound
    | exact ValTy.errorV ValTy.notAllowed
    | exact ValTy.bool
    | exact ValTy.cap
    | exact okTime_safe
    | exact okInt_safe
    | trivial

/-! ## Every primitive -/

/-- **Every primitive respects its signature**, in every program. -/
theorem prims_safe : PrimsSafe P := by
  intro p
  cases p with
  | int op => exact intOp_safe op
  | big op => exact bigOp_safe op
  | byte op => exact byteOp_safe op
  | dec op => exact decOp_safe op
  | float op => exact floatOp_safe op
  | not => exact not_safe
  | cmp b op => exact cmp_safe b op
  | char op => exact charOp_safe op
  | text op => exact textOp_safe op
  | bytes op => exact bytesOp_safe op
  | range op => exact rangeOp_safe op
  | time op => exact timeOp_safe op
  | list t op => exact listOp_safe t op
  | map k t op => exact mapOp_safe k t op
  | set t op => exact setOp_safe t op
  | eq t => exact eq_safe t
  | io op => exact ioOp_safe op

end MoCore
