/-
  The saved value format (docs/design/saved-state.md, "Encoded values") and the round trip
  replay depends on (docs/design/introspection.md, section 11.1).

  This is a model of its own, not built on Core's `Ty` and `ValTy`: the saved format tells a
  record from a choice, carries nominal names and type parameters, and refuses a map with a key
  twice, none of which Core's types say. It covers every type tag of the format, 0 to 18, and
  the back-reference byte 255 for a type repeated in a schema. It does not cover recursive
  schemas, so a value tree here is always finite in its type as well as in itself.

  Leaf payloads are modelled as the bytes the format lays out for them (an `Int` is its eight
  little-endian bytes, a `Text` its UTF-8 bytes), with the format's validity rules: a `Bool` is
  0 or 1, a `Char` a Unicode scalar, a `Time` within years 1 to 9999, a `Decimal` not the
  reserved minimum, a `BigInt` canonical. The theorems are about everything above the leaves:
  the schema check, counts and lengths, choice indexes, distinct keys and elements, nesting.

  The file has the specification first (the schema, values and when a value fits a schema),
  then the encoder and decoder, then the theorems, then the negative control: a decoder that
  skips the schema check accepts a value written under another schema.

  Every theorem's axioms are pinned with `#guard_msgs in #print axioms`.
-/

namespace MoCore.Saved

abbrev Bytes := List UInt8

/-! ## Specification -/

/-- The leaf types, tags 0 to 12 in tag order. -/
inductive Leaf where
  | unit | bool | byte | int | float | char | time
  | duration | decimal | range | text | bytes | bigInt
  deriving DecidableEq, Repr

/-- A saved schema: the leaves and tags 13 to 18. A record and a choice carry their nominal name
    and type parameters; a record's fields and each choice case's fields are named (an absent
    name is the empty string); a choice case says whether its fields are unnamed. -/
inductive Schema where
  | leaf (l : Leaf)
  | tuple (elements : List Schema)
  | list (element : Schema)
  | set (element : Schema)
  | map (key value : Schema)
  | record (name : String) (params : List Schema) (fields : List (String × Schema))
  | choice (name : String) (params : List Schema)
      (cases : List (String × Bool × List (String × Schema)))
  deriving BEq, Repr

/-- A value as the decoder builds it. A tuple's elements, a record's fields, a list's and a
    set's elements are all a sequence; the schema says which. -/
inductive Val where
  | leaf (payload : Bytes)
  | bigInt (sign : UInt8) (magnitude : Bytes)
  | seq (items : List Val)
  | map (entries : List (Val × Val))
  | case (index : Nat) (fields : List Val)
  deriving BEq, Repr

/-- The number little-endian bytes spell. -/
def unsignedLE : Bytes → Nat
  | [] => 0
  | b :: bs => b.toNat + 256 * unsignedLE bs

/-- The two's-complement number little-endian bytes spell. -/
def signedLE (bs : Bytes) : Int :=
  if unsignedLE bs < 2 ^ (8 * bs.length - 1) then unsignedLE bs
  else (unsignedLE bs : Int) - 2 ^ (8 * bs.length)

/-- `MO_TIME_MIN_US` and `MO_TIME_MAX_US`: years 1 and 9999 in UTC microseconds. -/
def timeMin : Int := -62135596800000000
def timeMax : Int := 253402300799999999

/-- The payloads the format accepts for each leaf (saved-state.md, value layouts). -/
def leafValid : Leaf → Bytes → Bool
  | .unit, bs => bs.isEmpty
  | .bool, bs => bs.length == 1 && decide (unsignedLE bs ≤ 1)
  | .byte, bs => bs.length == 1
  | .char, bs => bs.length == 4 && decide (unsignedLE bs < 0x110000) &&
      !(decide (0xD800 ≤ unsignedLE bs) && decide (unsignedLE bs ≤ 0xDFFF))
  | .int, bs | .float, bs | .duration, bs => bs.length == 8
  | .time, bs => bs.length == 8 && decide (timeMin ≤ signedLE bs) && decide (signedLE bs ≤ timeMax)
  | .decimal, bs => bs.length == 16 && unsignedLE bs != 2 ^ 127
  | .range, bs => bs.length == 16
  | .text, bs => decide (bs.length < 2 ^ 32) && ByteArray.validateUTF8 ⟨bs.toArray⟩
  | .bytes, bs => decide (bs.length < 2 ^ 32)
  | .bigInt, _ => false

/-- A canonical big integer: sign 0 (zero), 1 (positive) or 2 (negative); zero has an empty
    magnitude; a nonzero magnitude has a nonzero last byte. -/
def bigIntValid (sign : UInt8) (magnitude : Bytes) : Bool :=
  decide (sign.toNat ≤ 2) && (sign == 0) == magnitude.isEmpty &&
    magnitude.getLast? != some 0 && decide (magnitude.length < 2 ^ 32)

def FitsLeaf : Leaf → Val → Prop
  | .bigInt, .bigInt sign magnitude => bigIntValid sign magnitude = true
  | l, .leaf payload => leafValid l payload = true
  | _, _ => False

mutual
/-- A value fits a schema: it has the schema's shape, its leaves are valid, a set's elements
    and a map's keys are distinct, and every count fits the format's four-byte count fields. -/
def Fits : Schema → Val → Prop
  | .leaf l, v => FitsLeaf l v
  | .tuple ts, .seq vs => vs.length < 2 ^ 32 ∧ FitsAll ts vs
  | .list t, .seq vs => vs.length < 2 ^ 32 ∧ ∀ v ∈ vs, Fits t v
  | .set t, .seq vs => vs.length < 2 ^ 32 ∧ vs.Nodup ∧ ∀ v ∈ vs, Fits t v
  | .map k w, .map kvs =>
      kvs.length < 2 ^ 32 ∧ (kvs.map Prod.fst).Nodup ∧ ∀ e ∈ kvs, Fits k e.1 ∧ Fits w e.2
  | .record _ _ fs, .seq vs => vs.length < 2 ^ 32 ∧ FitsFields fs vs
  | .choice _ _ cs, .case i vs => i < 2 ^ 32 ∧ vs.length < 2 ^ 32 ∧ FitsCase cs i vs
  | _, _ => False
def FitsAll : List Schema → List Val → Prop
  | [], [] => True
  | t :: ts, v :: vs => Fits t v ∧ FitsAll ts vs
  | _, _ => False
def FitsFields : List (String × Schema) → List Val → Prop
  | [], [] => True
  | (_, t) :: fs, v :: vs => Fits t v ∧ FitsFields fs vs
  | _, _ => False
/-- The fields fit the case the index selects. -/
def FitsCase : List (String × Bool × List (String × Schema)) → Nat → List Val → Prop
  | (_, _, fs) :: _, 0, vs => FitsFields fs vs
  | _ :: cs, i + 1, vs => FitsCase cs i vs
  | [], _, _ => False
end


/-! ## Encoding -/

/-- A four-byte little-endian count. -/
def u32 (n : Nat) : Bytes :=
  [UInt8.ofNat n, UInt8.ofNat (n / 2 ^ 8), UInt8.ofNat (n / 2 ^ 16), UInt8.ofNat (n / 2 ^ 24)]

def Leaf.tag : Leaf → UInt8
  | .unit => 0 | .bool => 1 | .byte => 2 | .int => 3 | .float => 4 | .char => 5 | .time => 6
  | .duration => 7 | .decimal => 8 | .range => 9 | .text => 10 | .bytes => 11 | .bigInt => 12

/-- A name: its UTF-8 byte length, then its bytes. -/
def name (s : String) : Bytes := u32 s.utf8ByteSize ++ s.toUTF8.toList

/-- A type seen before (here: structurally equal to one seen before) is byte 255 and its
    traversal id; a first visit takes the next id before its children are visited, and writes
    the rest. -/
def visit (seen : List Schema) (τ : Schema) (rest : List Schema → Bytes × List Schema) :
    Bytes × List Schema :=
  match seen.idxOf? τ with
  | some id => (255 :: u32 id, seen)
  | none => rest (seen ++ [τ])

mutual
/-- The schema bytes: tag, a record's or choice's name, the type parameters, the groups (a
    choice's cases, one otherwise), and each group's fields. -/
def ser (seen : List Schema) : Schema → Bytes × List Schema
  | .leaf l => visit seen (.leaf l) fun seen => (l.tag :: u32 0 ++ u32 1 ++ u32 0, seen)
  | .tuple ts => visit seen (.tuple ts) fun seen =>
      let (ps, seen) := serAll seen ts
      (13 :: u32 ts.length ++ ps ++ u32 1 ++ u32 0, seen)
  | .list t => visit seen (.list t) fun seen =>
      let (p, seen) := ser seen t
      (14 :: u32 1 ++ p ++ u32 1 ++ u32 0, seen)
  | .set t => visit seen (.set t) fun seen =>
      let (p, seen) := ser seen t
      (15 :: u32 1 ++ p ++ u32 1 ++ u32 0, seen)
  | .map k w => visit seen (.map k w) fun seen =>
      let (pk, seen) := ser seen k
      let (pw, seen) := ser seen w
      (16 :: u32 2 ++ pk ++ pw ++ u32 1 ++ u32 0, seen)
  | .record n ps fs => visit seen (.record n ps fs) fun seen =>
      let (pb, seen) := serAll seen ps
      let (fb, seen) := serFields seen fs
      (17 :: name n ++ u32 ps.length ++ pb ++ u32 1 ++ u32 fs.length ++ fb, seen)
  | .choice n ps cs => visit seen (.choice n ps cs) fun seen =>
      let (pb, seen) := serAll seen ps
      let (cb, seen) := serCases seen cs
      (18 :: name n ++ u32 ps.length ++ pb ++ u32 cs.length ++ cb, seen)
def serAll (seen : List Schema) : List Schema → Bytes × List Schema
  | [] => ([], seen)
  | t :: ts =>
    let (b, seen) := ser seen t
    let (bs, seen) := serAll seen ts
    (b ++ bs, seen)
def serFields (seen : List Schema) : List (String × Schema) → Bytes × List Schema
  | [] => ([], seen)
  | (n, t) :: fs =>
    let (b, seen) := ser seen t
    let (bs, seen) := serFields seen fs
    (name n ++ b ++ bs, seen)
def serCases (seen : List Schema) : List (String × Bool × List (String × Schema)) →
    Bytes × List Schema
  | [] => ([], seen)
  | (n, unnamed, fs) :: cs =>
    let (b, seen) := serFields seen fs
    let (bs, seen) := serCases seen cs
    (name n ++ [if unnamed then 1 else 0] ++ u32 fs.length ++ b ++ bs, seen)
end


def schemaBytes (τ : Schema) : Bytes := (ser [] τ).1

/-- A leaf payload's fixed width; text, bytes and big integers carry their length instead. -/
def Leaf.width : Leaf → Option Nat
  | .unit => some 0 | .bool => some 1 | .byte => some 1 | .char => some 4
  | .int => some 8 | .float => some 8 | .time => some 8 | .duration => some 8
  | .decimal => some 16 | .range => some 16
  | .text | .bytes | .bigInt => none

def encLeaf (l : Leaf) : Val → Bytes
  | .bigInt sign magnitude => sign :: (u32 magnitude.length ++ magnitude)
  | .leaf payload =>
    match l.width with
    | some _ => payload
    | none => u32 payload.length ++ payload
  | _ => []

mutual
/-- A value's payload bytes under a schema. -/
def enc : Schema → Val → Bytes
  | .leaf l, v => encLeaf l v
  | .tuple ts, .seq vs => u32 vs.length ++ encAll ts vs
  | .list t, .seq vs => u32 vs.length ++ vs.flatMap (enc t)
  | .set t, .seq vs => u32 vs.length ++ vs.flatMap (enc t)
  | .map k w, .map kvs => u32 kvs.length ++ kvs.flatMap fun e => enc k e.1 ++ enc w e.2
  | .record _ _ fs, .seq vs => u32 vs.length ++ encFields fs vs
  | .choice _ _ cs, .case i vs => u32 i ++ u32 vs.length ++ encCase cs i vs
  | _, _ => []
def encAll : List Schema → List Val → Bytes
  | t :: ts, v :: vs => enc t v ++ encAll ts vs
  | _, _ => []
def encFields : List (String × Schema) → List Val → Bytes
  | (_, t) :: fs, v :: vs => enc t v ++ encFields fs vs
  | _, _ => []
def encCase : List (String × Bool × List (String × Schema)) → Nat → List Val → Bytes
  | (_, _, fs) :: _, 0, vs => encFields fs vs
  | _ :: cs, i + 1, vs => encCase cs i vs
  | [], _, _ => []
end


/-- `MOSV` version 1. -/
def magic : Bytes := [0x4d, 0x4f, 0x53, 0x56, 0x01]

/-- An encoded value: magic, the schema's length and bytes, the payload. -/
def encode (τ : Schema) (v : Val) : Bytes :=
  magic ++ u32 (schemaBytes τ).length ++ schemaBytes τ ++ enc τ v

/-! ## Decoding -/

def readU32 : Bytes → Option (Nat × Bytes)
  | a :: b :: c :: d :: rest =>
    some (a.toNat + 2 ^ 8 * b.toNat + 2 ^ 16 * c.toNat + 2 ^ 24 * d.toNat, rest)
  | _ => none

def readN (n : Nat) (bs : Bytes) : Option (Bytes × Bytes) :=
  if n ≤ bs.length then some (bs.take n, bs.drop n) else none

def readMany (f : Bytes → Option (α × Bytes)) : Nat → Bytes → Option (List α × Bytes)
  | 0, bs => some ([], bs)
  | n + 1, bs =>
    (f bs).bind fun (a, bs) => (readMany f n bs).map fun (as, bs) => (a :: as, bs)

def decLeaf (l : Leaf) (bs : Bytes) : Option (Val × Bytes) :=
  match l, bs with
  | .bigInt, sign :: bs =>
    (readU32 bs).bind fun (n, bs) => (readN n bs).bind fun (magnitude, bs) =>
      if bigIntValid sign magnitude then some (.bigInt sign magnitude, bs) else none
  | .bigInt, [] => none
  | l, bs =>
    let read := match l.width with
      | some w => readN w bs
      | none => (readU32 bs).bind fun (n, bs) => readN n bs
    read.bind fun (payload, bs) => if leafValid l payload then some (.leaf payload, bs) else none

mutual
/-- Decode a payload under the expected schema, returning what follows it. A set's elements and
    a map's keys are checked distinct by their encodings, which is distinctness of the values:
    the round trip below makes the encoding injective on values that fit. -/
def dec : Schema → Bytes → Option (Val × Bytes)
  | .leaf l, bs => decLeaf l bs
  | .tuple ts, bs => (readU32 bs).bind fun (n, bs) =>
      if n = ts.length then (decAll ts bs).map fun (vs, bs) => (.seq vs, bs) else none
  | .list t, bs => (readU32 bs).bind fun (n, bs) =>
      (readMany (dec t) n bs).map fun (vs, bs) => (.seq vs, bs)
  | .set t, bs => (readU32 bs).bind fun (n, bs) =>
      (readMany (dec t) n bs).bind fun (vs, bs) =>
        if (vs.map (enc t)).Nodup then some (.seq vs, bs) else none
  | .map k w, bs => (readU32 bs).bind fun (n, bs) =>
      (readMany (fun bs => (dec k bs).bind fun (a, bs) => (dec w bs).map fun (b, bs) => ((a, b), bs))
        n bs).bind fun (kvs, bs) =>
        if (kvs.map fun e => enc k e.1).Nodup then some (.map kvs, bs) else none
  | .record _ _ fs, bs => (readU32 bs).bind fun (n, bs) =>
      if n = fs.length then (decFields fs bs).map fun (vs, bs) => (.seq vs, bs) else none
  | .choice _ _ cs, bs => (readU32 bs).bind fun (i, bs) => (readU32 bs).bind fun (n, bs) =>
      (decCase cs i n bs).map fun (vs, bs) => (.case i vs, bs)
def decAll : List Schema → Bytes → Option (List Val × Bytes)
  | [], bs => some ([], bs)
  | t :: ts, bs => (dec t bs).bind fun (v, bs) => (decAll ts bs).map fun (vs, bs) => (v :: vs, bs)
def decFields : List (String × Schema) → Bytes → Option (List Val × Bytes)
  | [], bs => some ([], bs)
  | (_, t) :: fs, bs =>
    (dec t bs).bind fun (v, bs) => (decFields fs bs).map fun (vs, bs) => (v :: vs, bs)
/-- The fields of the case the index selects, after checking the field count. -/
def decCase : List (String × Bool × List (String × Schema)) → Nat → Nat → Bytes →
    Option (List Val × Bytes)
  | (_, _, fs) :: _, 0, n, bs => if n = fs.length then decFields fs bs else none
  | _ :: cs, i + 1, n, bs => decCase cs i n bs
  | [], _, _, _ => none
end


/-- The schema bytes of an encoded value, and its payload. -/
def schemaOf (b : Bytes) : Option (Bytes × Bytes) :=
  if b.take 5 = magic then (readU32 (b.drop 5)).bind fun (n, rest) => readN n rest else none

/-- Decode an encoded value: its schema bytes must equal the expected schema's, byte for byte,
    and the payload must be exactly one value of that schema. -/
def decode (τ : Schema) (b : Bytes) : Option Val :=
  (schemaOf b).bind fun (s, payload) =>
    if s = schemaBytes τ then
      match dec τ payload with
      | some (v, []) => some v
      | _ => none
    else none

/-! ## Lemmas on the byte readers -/

theorem readU32_u32 {n : Nat} (h : n < 2 ^ 32) (rest : Bytes) :
    readU32 (u32 n ++ rest) = some (n, rest) := by
  simp only [u32, readU32, List.cons_append, List.nil_append, UInt8.toNat_ofNat', Nat.reducePow]
  congr 2
  omega

/--
info: 'MoCore.Saved.readU32_u32' depends on axioms: [propext, Quot.sound]
-/
#guard_msgs in #print axioms readU32_u32

theorem readN_append (l rest : Bytes) : readN l.length (l ++ rest) = some (l, rest) := by
  simp [readN]

/--
info: 'MoCore.Saved.readN_append' depends on axioms: [propext]
-/
#guard_msgs in #print axioms readN_append

theorem readMany_flatMap {f : Bytes → Option (α × Bytes)} {g : α → Bytes} :
    ∀ (xs : List α) (rest : Bytes), (∀ x ∈ xs, ∀ r, f (g x ++ r) = some (x, r)) →
      readMany f xs.length (xs.flatMap g ++ rest) = some (xs, rest)
  | [], _, _ => rfl
  | x :: xs, rest, h => by
    simp only [List.length_cons, readMany, List.flatMap_cons, List.append_assoc,
      h x List.mem_cons_self, Option.bind_some,
      readMany_flatMap xs rest (fun y hy => h y (List.mem_cons_of_mem _ hy)), Option.map_some]

/--
info: 'MoCore.Saved.readMany_flatMap' depends on axioms: [propext]
-/
#guard_msgs in #print axioms readMany_flatMap

theorem nodup_map_of_injective {f : α → β} :
    ∀ {l : List α}, (∀ x ∈ l, ∀ y ∈ l, f x = f y → x = y) → l.Nodup → (l.map f).Nodup
  | [], _, _ => List.nodup_nil
  | a :: l, h, hn => by
    rw [List.nodup_cons] at hn
    rw [List.map_cons, List.nodup_cons]
    refine ⟨fun hm => ?_, nodup_map_of_injective
      (fun x hx y hy => h x (List.mem_cons_of_mem _ hx) y (List.mem_cons_of_mem _ hy)) hn.2⟩
    obtain ⟨b, hb, he⟩ := List.mem_map.1 hm
    exact hn.1 (h b (List.mem_cons_of_mem _ hb) a List.mem_cons_self he ▸ hb)

/--
info: 'MoCore.Saved.nodup_map_of_injective' depends on axioms: [propext, Quot.sound]
-/
#guard_msgs in #print axioms nodup_map_of_injective

/-! ## Leaves -/

theorem leafValid_width {l : Leaf} {p : Bytes} {w : Nat} (h : leafValid l p = true)
    (hw : l.width = some w) : p.length = w := by
  cases l <;> simp [Leaf.width] at hw <;> subst hw <;> simp [leafValid] at h <;>
    first | (subst h; rfl) | omega | exact h.1 | exact h.1.1 | exact h.1.1.1

/--
info: 'MoCore.Saved.leafValid_width' depends on axioms: [propext, Classical.choice, Quot.sound]
-/
#guard_msgs in #print axioms leafValid_width

theorem leafValid_length {l : Leaf} {p : Bytes} (h : leafValid l p = true)
    (hw : l.width = none) : p.length < 2 ^ 32 := by
  cases l <;> simp [Leaf.width] at hw <;> simp [leafValid] at h <;> first | exact h | exact h.1

/--
info: 'MoCore.Saved.leafValid_length' depends on axioms: [propext, Classical.choice, Quot.sound]
-/
#guard_msgs in #print axioms leafValid_length

/-- Reading a leaf other than a big integer: its payload, then the validity check. -/
theorem decLeaf_eq {l : Leaf} (hl : l ≠ .bigInt) (bs : Bytes) :
    decLeaf l bs =
      ((match l.width with
        | some w => readN w bs
        | none => (readU32 bs).bind fun (n, bs) => readN n bs).bind fun (payload, bs) =>
        if leafValid l payload then some (.leaf payload, bs) else none) := by
  cases l <;> first | exact absurd rfl hl | rfl

/--
info: 'MoCore.Saved.decLeaf_eq' depends on axioms: [propext, Classical.choice, Quot.sound]
-/
#guard_msgs in #print axioms decLeaf_eq

theorem decLeaf_encLeaf (l : Leaf) (v : Val) (rest : Bytes) (h : FitsLeaf l v) :
    decLeaf l (encLeaf l v ++ rest) = some (v, rest) := by
  cases v with
  | bigInt sign magnitude =>
    cases l <;> simp only [FitsLeaf] at h
    have hm : magnitude.length < 2 ^ 32 := by simp [bigIntValid] at h; omega
    simp [encLeaf, decLeaf, readU32_u32 hm, readN, h]
  | leaf payload =>
    simp only [FitsLeaf] at h
    have hb : l ≠ .bigInt := by rintro rfl; simp [leafValid] at h
    rw [decLeaf_eq hb]
    cases hw : l.width with
    | some w =>
      have hl := leafValid_width h hw
      subst hl
      simp [encLeaf, hw, readN_append, h]
    | none =>
      have hl := leafValid_length h hw
      simp [encLeaf, hw, List.append_assoc, readU32_u32 hl, readN_append, h]
  | seq | map | case => cases l <;> simp [FitsLeaf] at h

/--
info: 'MoCore.Saved.decLeaf_encLeaf' depends on axioms: [propext, Classical.choice, Quot.sound]
-/
#guard_msgs in #print axioms decLeaf_encLeaf

/-! ## The payload round trip -/

theorem fitsAll_length : ∀ {ts : List Schema} {vs : List Val}, FitsAll ts vs → vs.length = ts.length
  | [], [], _ => rfl
  | _ :: _, _ :: _, h => by simp [fitsAll_length h.2]
  | [], _ :: _, h | _ :: _, [], h => by simp [FitsAll] at h

/--
info: 'MoCore.Saved.fitsAll_length' depends on axioms: [propext, Classical.choice, Quot.sound]
-/
#guard_msgs in #print axioms fitsAll_length

theorem fitsFields_length :
    ∀ {fs : List (String × Schema)} {vs : List Val}, FitsFields fs vs → vs.length = fs.length
  | [], [], _ => rfl
  | _ :: _, _ :: _, h => by simp [fitsFields_length h.2]
  | [], _ :: _, h | _ :: _, [], h => by simp [FitsFields] at h

mutual
/-- Decoding a fitting value's payload gives the value back and leaves what follows. -/
theorem dec_enc : ∀ (τ : Schema) (v : Val) (rest : Bytes), Fits τ v →
    dec τ (enc τ v ++ rest) = some (v, rest)
  | .leaf l, v, rest, h => by
    simp only [dec, enc]
    exact decLeaf_encLeaf l v rest h
  | .tuple ts, v, rest, h => by
    cases v <;> simp only [Fits] at h
    rename_i vs
    obtain ⟨hn, ha⟩ := h
    have hl := fitsAll_length ha
    have hn' : ts.length < 2 ^ 32 := by rw [← hl]; exact hn
    simp [dec, enc, List.append_assoc, hl, readU32_u32 hn', decAll_encAll ts vs rest ha]
  | .list t, v, rest, h => by
    cases v <;> simp only [Fits] at h
    rename_i vs
    obtain ⟨hn, ha⟩ := h
    simp only [dec, enc, List.append_assoc, readU32_u32 hn, Option.bind_some,
      readMany_flatMap vs rest (fun v hv r => dec_enc t v r (ha v hv)), Option.map_some]
  | .set t, v, rest, h => by
    cases v <;> simp only [Fits] at h
    rename_i vs
    obtain ⟨hn, hd, ha⟩ := h
    have hinj : ∀ x ∈ vs, ∀ y ∈ vs, enc t x = enc t y → x = y := fun x hx y hy he => by
      have h1 := dec_enc t x [] (ha x hx)
      have h2 := dec_enc t y [] (ha y hy)
      rw [he] at h1
      exact (Prod.mk.inj (Option.some.inj (h1.symm.trans h2))).1
    simp only [dec, enc, List.append_assoc, readU32_u32 hn, Option.bind_some,
      readMany_flatMap vs rest (fun v hv r => dec_enc t v r (ha v hv)),
      nodup_map_of_injective hinj hd, ite_true]
  | .map k w, v, rest, h => by
    cases v <;> simp only [Fits] at h
    rename_i kvs
    obtain ⟨hn, hd, ha⟩ := h
    have hpair : ∀ e ∈ kvs, ∀ r,
        ((dec k (enc k e.1 ++ enc w e.2 ++ r)).bind fun (a, bs) =>
          (dec w bs).map fun (b, bs) => ((a, b), bs)) = some (e, r) := fun e he r => by
      simp [List.append_assoc, dec_enc k e.1 _ (ha e he).1, dec_enc w e.2 r (ha e he).2]
    have hinj : ∀ x ∈ kvs.map Prod.fst, ∀ y ∈ kvs.map Prod.fst, enc k x = enc k y → x = y :=
      fun x hx y hy he => by
        obtain ⟨ex, hex, rfl⟩ := List.mem_map.1 hx
        obtain ⟨ey, hey, rfl⟩ := List.mem_map.1 hy
        have h1 := dec_enc k ex.1 [] (ha ex hex).1
        have h2 := dec_enc k ey.1 [] (ha ey hey).1
        rw [he] at h1
        exact (Prod.mk.inj (Option.some.inj (h1.symm.trans h2))).1
    have hkeys : (kvs.map fun e => enc k e.1).Nodup := by
      have := nodup_map_of_injective hinj hd
      rwa [List.map_map] at this
    simp only [dec, enc, List.append_assoc, readU32_u32 hn, Option.bind_some]
    rw [readMany_flatMap kvs rest hpair]
    simp only [Option.bind_some, hkeys, ite_true]
  | .record _ _ fs, v, rest, h => by
    cases v <;> simp only [Fits] at h
    rename_i vs
    obtain ⟨hn, ha⟩ := h
    have hl := fitsFields_length ha
    have hn' : fs.length < 2 ^ 32 := by rw [← hl]; exact hn
    simp [dec, enc, List.append_assoc, hl, readU32_u32 hn', decFields_encFields fs vs rest ha]
  | .choice _ _ cs, v, rest, h => by
    cases v <;> simp only [Fits] at h
    rename_i i vs
    obtain ⟨hi, hn, ha⟩ := h
    simp only [dec, enc, List.append_assoc, readU32_u32 hi, readU32_u32 hn, Option.bind_some,
      decCase_encCase cs i vs rest ha, Option.map_some]
theorem decAll_encAll : ∀ (ts : List Schema) (vs : List Val) (rest : Bytes), FitsAll ts vs →
    decAll ts (encAll ts vs ++ rest) = some (vs, rest)
  | [], [], rest, _ => rfl
  | t :: ts, v :: vs, rest, h => by
    simp only [encAll, decAll, List.append_assoc, dec_enc t v _ h.1, Option.bind_some,
      decAll_encAll ts vs rest h.2, Option.map_some]
  | [], _ :: _, _, h | _ :: _, [], _, h => by simp [FitsAll] at h
theorem decFields_encFields : ∀ (fs : List (String × Schema)) (vs : List Val) (rest : Bytes),
    FitsFields fs vs → decFields fs (encFields fs vs ++ rest) = some (vs, rest)
  | [], [], rest, _ => rfl
  | (_, t) :: fs, v :: vs, rest, h => by
    simp only [encFields, decFields, List.append_assoc, dec_enc t v _ h.1, Option.bind_some,
      decFields_encFields fs vs rest h.2, Option.map_some]
  | [], _ :: _, _, h | _ :: _, [], _, h => by simp [FitsFields] at h
theorem decCase_encCase : ∀ (cs : List (String × Bool × List (String × Schema))) (i : Nat)
    (vs : List Val) (rest : Bytes), FitsCase cs i vs →
    decCase cs i vs.length (encCase cs i vs ++ rest) = some (vs, rest)
  | (_, _, fs) :: _, 0, vs, rest, h => by
    simp only [FitsCase] at h
    simp [encCase, decCase, fitsFields_length h, decFields_encFields fs vs rest h]
  | _ :: cs, i + 1, vs, rest, h => by
    simp only [FitsCase] at h
    simp only [encCase, decCase]
    exact decCase_encCase cs i vs rest h
  | [], _, _, _, h => by simp [FitsCase] at h
end

/--
info: 'MoCore.Saved.fitsFields_length' depends on axioms: [propext, Classical.choice, Quot.sound]
-/
#guard_msgs in #print axioms fitsFields_length

/--
info: 'MoCore.Saved.dec_enc' depends on axioms: [propext, Classical.choice, Quot.sound]
-/
#guard_msgs in #print axioms dec_enc

/--
info: 'MoCore.Saved.decAll_encAll' depends on axioms: [propext, Classical.choice, Quot.sound]
-/
#guard_msgs in #print axioms decAll_encAll

/--
info: 'MoCore.Saved.decFields_encFields' depends on axioms: [propext, Classical.choice, Quot.sound]
-/
#guard_msgs in #print axioms decFields_encFields

/--
info: 'MoCore.Saved.decCase_encCase' depends on axioms: [propext, Classical.choice, Quot.sound]
-/
#guard_msgs in #print axioms decCase_encCase


/-! ## Encoded values -/

theorem schemaOf_encode {τ : Schema} (h : (schemaBytes τ).length < 2 ^ 32) (v : Val) :
    schemaOf (encode τ v) = some (schemaBytes τ, enc τ v) := by
  simp [schemaOf, encode, magic, List.append_assoc, readU32_u32 h, readN]

/--
info: 'MoCore.Saved.schemaOf_encode' depends on axioms: [propext, Quot.sound]
-/
#guard_msgs in #print axioms schemaOf_encode

/-- Reconstruction preserves values: every value that fits a schema decodes, from its encoding
    under that schema, to itself. The hypothesis on the schema is that its bytes fit the
    four-byte length before them. -/
theorem decode_encode {τ : Schema} {v : Val} (hs : (schemaBytes τ).length < 2 ^ 32)
    (h : Fits τ v) : decode τ (encode τ v) = some v := by
  have hp := dec_enc τ v [] h
  rw [List.append_nil] at hp
  simp [decode, schemaOf_encode hs, hp]

/--
info: 'MoCore.Saved.decode_encode' depends on axioms: [propext, Classical.choice, Quot.sound]
-/
#guard_msgs in #print axioms decode_encode

/-- A decoder checks the schema when it decodes only bytes whose schema is the expected one. -/
def ChecksSchema (d : Schema → Bytes → Option Val) : Prop :=
  ∀ τ b v, d τ b = some v → ∃ payload, schemaOf b = some (schemaBytes τ, payload)

/-- The decoder refuses bytes whose schema differs from the expected schema. -/
theorem decode_checks_schema : ChecksSchema decode := by
  intro τ b v h
  unfold decode at h
  cases hs : schemaOf b with
  | none => simp [hs] at h
  | some p =>
    obtain ⟨s, payload⟩ := p
    by_cases he : s = schemaBytes τ
    · subst he; exact ⟨payload, rfl⟩
    · simp [hs, he] at h

/--
info: 'MoCore.Saved.decode_checks_schema' depends on axioms: [propext, Classical.choice, Quot.sound]
-/
#guard_msgs in #print axioms decode_checks_schema

/-- A value written under one schema is refused under any schema with other bytes. -/
theorem decode_refuses_other_schema {τ τ' : Schema} (hs : (schemaBytes τ').length < 2 ^ 32)
    (hne : schemaBytes τ' ≠ schemaBytes τ) (v : Val) : decode τ (encode τ' v) = none := by
  cases hd : decode τ (encode τ' v) with
  | none => rfl
  | some u =>
    obtain ⟨_, hp⟩ := decode_checks_schema τ _ u hd
    rw [schemaOf_encode hs] at hp
    exact absurd (Prod.mk.inj (Option.some.inj hp)).1 hne

/--
info: 'MoCore.Saved.decode_refuses_other_schema' depends on axioms: [propext, Classical.choice, Quot.sound]
-/
#guard_msgs in #print axioms decode_refuses_other_schema

/-! ## A failure's replay inputs

  A failure record stores the encoded pre-call state in its state row, and in its message the
  arrival time (a `Time` payload), the argument count and each argument as a four-byte length
  and a complete encoded value, as a log record does. The message's other fields (ids, names,
  the signature, budgets, identities, context) are not handler inputs and are left out here. -/

structure Inputs where
  state : Val
  time : Val
  arguments : List Val
  deriving BEq, Repr

def encodeArgs : List Schema → List Val → Bytes
  | τ :: ts, a :: as => u32 (encode τ a).length ++ encode τ a ++ encodeArgs ts as
  | _, _ => []

def decodeArgs : List Schema → Bytes → Option (List Val × Bytes)
  | [], bs => some ([], bs)
  | τ :: ts, bs => (readU32 bs).bind fun (n, bs) => (readN n bs).bind fun (chunk, bs) =>
      (decode τ chunk).bind fun a => (decodeArgs ts bs).map fun (as, bs) => (a :: as, bs)

/-- The state row and the message's input fields. -/
def encodeInputs (state : Schema) (args : List Schema) (i : Inputs) : Bytes × Bytes :=
  (encode state i.state,
   enc (.leaf .time) i.time ++ u32 i.arguments.length ++ encodeArgs args i.arguments)

def decodeInputs (state : Schema) (args : List Schema) (record : Bytes × Bytes) :
    Option Inputs :=
  (decode state record.1).bind fun s => (dec (.leaf .time) record.2).bind fun (t, m) =>
    (readU32 m).bind fun (n, m) =>
      if n = args.length then
        match decodeArgs args m with
        | some (as, []) => some ⟨s, t, as⟩
        | _ => none
      else none

/-- Each argument fits its schema, and its encoded value fits the four-byte length before it. -/
def ArgsFit : List Schema → List Val → Prop
  | [], [] => True
  | τ :: ts, a :: as => Fits τ a ∧ (encode τ a).length < 2 ^ 32 ∧ ArgsFit ts as
  | _, _ => False

theorem argsFit_length : ∀ {ts : List Schema} {as : List Val}, ArgsFit ts as → as.length = ts.length
  | [], [], _ => rfl
  | _ :: _, _ :: _, h => by simp [argsFit_length h.2.2]
  | [], _ :: _, h | _ :: _, [], h => by simp [ArgsFit] at h

/--
info: 'MoCore.Saved.argsFit_length' depends on axioms: [propext, Classical.choice, Quot.sound]
-/
#guard_msgs in #print axioms argsFit_length

theorem decodeArgs_encodeArgs : ∀ (ts : List Schema) (as : List Val) (rest : Bytes),
    ArgsFit ts as → decodeArgs ts (encodeArgs ts as ++ rest) = some (as, rest)
  | [], [], rest, _ => rfl
  | τ :: ts, a :: as, rest, ⟨hf, hl, h⟩ => by
    have hs : (schemaBytes τ).length < 2 ^ 32 := by simp [encode] at hl; omega
    simp only [encodeArgs, decodeArgs, List.append_assoc, readU32_u32 hl, Option.bind_some,
      readN_append, decode_encode hs hf, decodeArgs_encodeArgs ts as rest h, Option.map_some]
  | [], _ :: _, _, h | _ :: _, [], _, h => by simp [ArgsFit] at h

/--
info: 'MoCore.Saved.decodeArgs_encodeArgs' depends on axioms: [propext, Classical.choice, Quot.sound]
-/
#guard_msgs in #print axioms decodeArgs_encodeArgs

/-- A failure's inputs, the state, the arrival time and the arguments, round-trip as a whole:
    replay runs the handler on exactly what was recorded. -/
theorem decodeInputs_encodeInputs (state : Schema) (args : List Schema) (i : Inputs)
    (hs : (schemaBytes state).length < 2 ^ 32) (hstate : Fits state i.state)
    (htime : Fits (.leaf .time) i.time) (hargs : ArgsFit args i.arguments)
    (hcount : i.arguments.length < 2 ^ 32) :
    decodeInputs state args (encodeInputs state args i) = some i := by
  have hl := argsFit_length hargs
  have hc : args.length < 2 ^ 32 := by rw [← hl]; exact hcount
  have ha := decodeArgs_encodeArgs _ _ [] hargs
  simp [decodeInputs, encodeInputs, decode_encode hs hstate, List.append_assoc,
    dec_enc _ _ _ htime, hl, readU32_u32 hc]
  rw [← List.append_nil (encodeArgs args i.arguments), ha]

/--
info: 'MoCore.Saved.decodeInputs_encodeInputs' depends on axioms: [propext, Classical.choice, Quot.sound]
-/
#guard_msgs in #print axioms decodeInputs_encodeInputs

/-! ## Checks of the model on concrete bytes -/

def account : Schema :=
  .record "Account" [.leaf .int] [("owner", .leaf .text), ("seen", .map (.leaf .int) (.leaf .int)),
    ("plan", .choice "Plan" [] [("Free", false, []), ("Paid", true, [("", .leaf .decimal)])])]

def int8 (n : Nat) : Val := .leaf ((u32 n) ++ u32 0)

-- A repeated type is written once and then referred back to: `Int` is traversal id 1 here.
#guard schemaBytes (.map (.leaf .int) (.leaf .int)) ==
  16 :: u32 2 ++ (3 :: u32 0 ++ u32 1 ++ u32 0) ++ (255 :: u32 1) ++ u32 1 ++ u32 0
#guard decode account (encode account (.seq [.leaf [0x6f, 0x6b], .map [(int8 1, int8 2)],
  .case 0 []])) == some (.seq [.leaf [0x6f, 0x6b], .map [(int8 1, int8 2)], .case 0 []])
-- A map with a key twice, a Bool of 2, text that is not UTF-8 and a choice index past the
-- cases are refused.
#guard (decode account (encode account (.seq [.leaf [], .map [(int8 1, int8 2), (int8 1, int8 3)],
  .case 0 []]))).isNone
#guard (decode (.leaf .bool) (encode (.leaf .bool) (.leaf [2]))).isNone
#guard (decode (.leaf .text) (encode (.leaf .text) (.leaf [0xff]))).isNone
#guard (decode account (encode account (.seq [.leaf [], .map [], .case 2 []]))).isNone
-- A record and a choice of the same name and fields have different schema bytes.
#guard schemaBytes (.record "T" [] [("a", .leaf .int)]) !=
  schemaBytes (.choice "T" [] [("a", false, [("", .leaf .int)])])

/-! ## Negative control

  A decoder that reads past the schema bytes without comparing them decodes a `Duration`'s
  encoding as an `Int`, so it fails the refusal half of the round trip. -/

def decodeBlind (τ : Schema) (b : Bytes) : Option Val :=
  (schemaOf b).bind fun (_, payload) =>
    match dec τ payload with
    | some (v, []) => some v
    | _ => none

#guard decodeBlind (.leaf .int) (encode (.leaf .duration) (int8 5)) == some (int8 5)
#guard (decode (.leaf .int) (encode (.leaf .duration) (int8 5))).isNone

theorem schemaBytes_leaf (l : Leaf) : schemaBytes (.leaf l) = l.tag :: (u32 0 ++ u32 1 ++ u32 0) := by
  simp [schemaBytes, ser, visit]

/--
info: 'MoCore.Saved.schemaBytes_leaf' depends on axioms: [propext, Quot.sound]
-/
#guard_msgs in #print axioms schemaBytes_leaf

theorem decodeBlind_ignores_schema : ¬ ChecksSchema decodeBlind := by
  have hs : (schemaBytes (.leaf .duration)).length < 2 ^ 32 := by
    rw [schemaBytes_leaf]; decide
  have hne : schemaBytes (.leaf .duration) ≠ schemaBytes (.leaf .int) := by
    rw [schemaBytes_leaf, schemaBytes_leaf]; decide
  have hdec : decodeBlind (.leaf .int) (encode (.leaf .duration) (int8 5)) = some (int8 5) := by
    simp only [decodeBlind, schemaOf_encode hs, Option.bind_some]
    rfl
  intro h
  obtain ⟨_, hp⟩ := h (.leaf .int) (encode (.leaf .duration) (int8 5)) (int8 5) hdec
  rw [schemaOf_encode hs] at hp
  exact absurd (Prod.mk.inj (Option.some.inj hp)).1 hne

/--
info: 'MoCore.Saved.decodeBlind_ignores_schema' depends on axioms: [propext, Classical.choice, Quot.sound]
-/
#guard_msgs in #print axioms decodeBlind_ignores_schema

/-! ## Migration boundary

The finite-schema model uses an explicit type-indexed table of nominal-name overrides.
The caller supplies the type table; NULL entries in C correspond to `none`. As elsewhere
in this file, recursive source declarations and resource accounting are outside this model.
Activation below models the validation/commit boundary, not SQLite or the C validator.
-/

def savedName (types : List Schema) (names : List (Option String)) (t : Schema)
    (original : String) : String :=
  match types.idxOf? t with
  | none => original
  | some i => match names[i]? with
    | some (some n) => n
    | _ => original

mutual
def overrideNames (types : List Schema) (names : List (Option String)) : Schema → Schema
  | .leaf l => .leaf l
  | .tuple ts => .tuple (overrideAll types names ts)
  | .list t => .list (overrideNames types names t)
  | .set t => .set (overrideNames types names t)
  | .map k v => .map (overrideNames types names k) (overrideNames types names v)
  | .record n ps fs => .record (savedName types names (.record n ps fs) n)
      (overrideAll types names ps) (overrideFields types names fs)
  | .choice n ps cs => .choice (savedName types names (.choice n ps cs) n)
      (overrideAll types names ps) (overrideCases types names cs)
def overrideAll (types : List Schema) (names : List (Option String)) : List Schema → List Schema
  | [] => []
  | t :: ts => overrideNames types names t :: overrideAll types names ts
def overrideFields (types : List Schema) (names : List (Option String)) :
    List (String × Schema) → List (String × Schema)
  | [] => []
  | (n, t) :: fs => (n, overrideNames types names t) :: overrideFields types names fs
def overrideCases (types : List Schema) (names : List (Option String)) :
    List (String × Bool × List (String × Schema)) →
    List (String × Bool × List (String × Schema))
  | [] => []
  | (n, unnamed, fs) :: cs =>
      (n, unnamed, overrideFields types names fs) :: overrideCases types names cs
end

/-- Overrides change only the schema expected at the input boundary; the ordinary decoder
    and its other-schema refusal theorem above remain unchanged. -/
def decodeMigrationInput (types : List Schema) (names : List (Option String))
    (t : Schema) (bytes : Bytes) : Option Val := decode (overrideNames types names t) bytes

theorem migration_input_checks_schema (types : List Schema) (names : List (Option String))
    (t : Schema) (bytes : Bytes) (v : Val)
    (h : decodeMigrationInput types names t bytes = some v) :
    ∃ payload, schemaOf bytes = some (schemaBytes (overrideNames types names t), payload) :=
  decode_checks_schema _ _ _ h

/--
info: 'MoCore.Saved.migration_input_checks_schema' depends on axioms: [propext, Classical.choice, Quot.sound]
-/
#guard_msgs in #print axioms migration_input_checks_schema

structure UpgradeStore where
  program : Bytes
  snapshot : Val
  log : List Bytes
  ordinal : Nat
  deriving BEq, Repr

/-- `candidate = none` includes a trap or exhausted budget. A successful migration still
    requires validation against the new, unmodified state schema before any store change. -/
noncomputable def activateMigration (old : UpgradeStore) (program : Bytes) (state : Schema)
    (candidate : Option Val) : Option UpgradeStore := by
  classical
  exact candidate.bind fun value =>
    if Fits state value then some ⟨program, value, [], old.ordinal + 1⟩ else none

def afterMigration (old : UpgradeStore) (activation : Option UpgradeStore) : UpgradeStore :=
  activation.getD old

theorem migration_refusal_preserves_store (old : UpgradeStore) :
    afterMigration old none = old := rfl

/--
info: 'MoCore.Saved.migration_refusal_preserves_store' does not depend on any axioms
-/
#guard_msgs in #print axioms migration_refusal_preserves_store

theorem migration_accepted_validates (old : UpgradeStore) (program : Bytes) (state : Schema)
    (candidate : Option Val) (next : UpgradeStore)
    (h : activateMigration old program state candidate = some next) : Fits state next.snapshot := by
  classical
  cases candidate with
  | none => simp [activateMigration] at h
  | some v =>
    simp only [activateMigration, Option.bind_some] at h
    split at h
    next valid => cases h; exact valid
    next invalid => simp at h

/--
info: 'MoCore.Saved.migration_accepted_validates' depends on axioms: [propext, Classical.choice, Quot.sound]
-/
#guard_msgs in #print axioms migration_accepted_validates

#guard overrideNames [.record "Old" [] []] [some "UpgradeOld.Old"] (.record "Old" [] []) ==
  .record "UpgradeOld.Old" [] []
#guard overrideNames [.record "Old" [] []] [some "UpgradeOld.Old"] (.record "Other" [] []) ==
  .record "Other" [] []

end MoCore.Saved
