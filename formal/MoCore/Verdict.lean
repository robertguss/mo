/-
  The verdict of `mo replay --fix` and the signature matching it rests on
  (docs/design/introspection.md, sections 6.2 and 7.1).

  The file has three parts. The specification states, as inductive relations, what the design's
  tables say: when two signatures match and which verdict each pair of outcomes gets. The
  functions are what the C code mirrors: `signaturesMatch` and `verdict`. The theorems prove the
  functions equal to the specification, and the negative control shows that a verdict which
  looks only at the revised run fails the soundness statement on a concrete pair.

  Every theorem's axioms are pinned with `#guard_msgs in #print axioms`, so a proof that starts
  to depend on `sorryAx` or on any axiom beyond `propext`, `Classical.choice` and `Quot.sound`
  fails the build; `ci/check_lean_axioms.sh` checks the pinned lists themselves.
-/

namespace MoCore.Verdict

/-! ## Outcomes and signatures -/

/-- The limit a memory failure names. Native allocation is a limit of its own, so a failure
    from it matches only another native allocation failure. -/
inductive MemoryLimit where
  | heap | stack | depth | nativeAllocation
  deriving DecidableEq, Repr

/-- The limit a failed saved copy of the new state names. -/
inductive SavedLimit where
  | bytes | nodes | depth
  deriving DecidableEq, Repr

/-- A failure's structured signature (design 6.2). A defect's reason is the VM's fixed text,
    never the program's own `stop` text; functions and files are table indexes and the span is
    a pair of byte offsets. A work failure carries its limit's name and value. -/
inductive Signature where
  | defect (reason : String) (function : Nat) (file : Nat) (span : Nat × Nat)
  | work (limit : String) (value : Nat)
  | memory (limit : MemoryLimit) (value : Nat)
  | stateSize (limit : SavedLimit) (value : Nat)
  deriving DecidableEq, Repr

/-- The reply a handler that returned gave. An `Error` reply is a success. -/
inductive Reply where
  | ok | error
  deriving DecidableEq, Repr

/-- The saved-copy size check of the new state: needed only by an updating tool or a timer. -/
inductive SizeCheck where
  | notNeeded
  | passed
  | failed (limit : SavedLimit) (value : Nat)
  deriving DecidableEq, Repr

/-- What one run of a handler gave: refused before anything ran, stopped with a signature, or
    returned (with its reply, whether the state changed, and the size check). -/
inductive Outcome where
  | refused
  | stopped (signature : Signature)
  | returned (reply : Reply) (stateChanged : Bool) (size : SizeCheck)
  deriving DecidableEq, Repr

/-- The four verdicts of design 7.1. -/
inductive Verdict where
  | verified | notFixed | unverified | refused
  deriving DecidableEq, Repr

/-! ## Specification

  Written from the design's tables, without reference to the functions below. -/

/-- Design 6.2: when a replay's signature matches the recorded one. A defect compares class,
    reason, function, file and span (a span locates code only within its file); work compares
    the limit's value; memory the
    limit's name and value; a state-size failure the same saved limit. -/
inductive Matches : Signature → Signature → Prop where
  | defect : Matches (.defect reason function file span) (.defect reason function file span)
  | work : Matches (.work name value) (.work name' value)
  | memory : Matches (.memory limit value) (.memory limit value)
  | stateSize : Matches (.stateSize limit value) (.stateSize limit value)

/-- The signature a run leaves, as capture records it: a stop's own, or, when the handler
    returned but its new state's saved copy failed, a state-size signature. -/
inductive Leaves : Outcome → Signature → Prop where
  | stopped : Leaves (.stopped s) s
  | sizeFailed : Leaves (.returned reply changed (.failed limit value)) (.stateSize limit value)

/-- The original run reproduced the recorded failure. -/
def Reproduced (recorded : Signature) (o : Outcome) : Prop :=
  ∃ s, Leaves o s ∧ Matches recorded s

/-- The run returned, with either reply. -/
inductive Succeeded : Outcome → Prop where
  | returned : Succeeded (.returned reply changed size)

/-- No saved-copy size check failed: the handler needed none, or its new state passed. -/
inductive SizeOk : Outcome → Prop where
  | stopped : SizeOk (.stopped s)
  | refused : SizeOk .refused
  | notNeeded : SizeOk (.returned reply changed .notNeeded)
  | passed : SizeOk (.returned reply changed .passed)

/-- Design 7.1's verdict table, row by row. A refusal of either program means nothing ran. -/
inductive Table (recorded : Signature) : Outcome → Outcome → Verdict → Prop where
  | refusedOriginal : Table recorded .refused r .refused
  | refusedRevised : Table recorded o .refused .refused
  | verified : Reproduced recorded o → Succeeded r → SizeOk r → Table recorded o r .verified
  | notFixed : Reproduced recorded o → r ≠ .refused → ¬ (Succeeded r ∧ SizeOk r) →
      Table recorded o r .notFixed
  | unverified : o ≠ .refused → r ≠ .refused → ¬ Reproduced recorded o →
      Table recorded o r .unverified

/-- The soundness statement of design 11.1, for any verdict function: `verified` exactly when
    the original reproduced and the revised run succeeded with no failed size check. -/
def Sound (f : Signature → Outcome → Outcome → Verdict) : Prop :=
  ∀ recorded o r, f recorded o r = .verified ↔ Reproduced recorded o ∧ Succeeded r ∧ SizeOk r

/-! ## The functions -/

def signaturesMatch : Signature → Signature → Bool
  | .defect r f file s, .defect r' f' file' s' => r == r' && f == f' && file == file' && s == s'
  | .work _ v, .work _ v' => v == v'
  | .memory l v, .memory l' v' => l == l' && v == v'
  | .stateSize l v, .stateSize l' v' => l == l' && v == v'
  | _, _ => false

/-- The signature an outcome leaves, if any. -/
def signature? : Outcome → Option Signature
  | .stopped s => some s
  | .returned _ _ (.failed l v) => some (.stateSize l v)
  | _ => none

def reproduced (recorded : Signature) (o : Outcome) : Bool :=
  match signature? o with
  | some s => signaturesMatch recorded s
  | none => false

/-- The revised run returned and no size check failed. -/
def passes : Outcome → Bool
  | .returned _ _ .notNeeded | .returned _ _ .passed => true
  | _ => false

def verdict (recorded : Signature) (o r : Outcome) : Verdict :=
  match o, r with
  | .refused, _ | _, .refused => .refused
  | _, _ =>
    if reproduced recorded o then (if passes r then .verified else .notFixed) else .unverified

/-! ## Matching -/

theorem signaturesMatch_iff (a b : Signature) : signaturesMatch a b = true ↔ Matches a b := by
  constructor
  · intro h
    cases a <;> cases b <;> simp [signaturesMatch] at h
    all_goals first
      | (obtain ⟨⟨⟨rfl, rfl⟩, rfl⟩, rfl⟩ := h; exact .defect)
      | (subst h; exact .work)
      | (obtain ⟨rfl, rfl⟩ := h; first | exact .memory | exact .stateSize)
  · intro h
    cases h <;> simp [signaturesMatch]

/--
info: 'MoCore.Verdict.signaturesMatch_iff' depends on axioms: [propext, Classical.choice, Quot.sound]
-/
#guard_msgs in #print axioms signaturesMatch_iff

theorem matches_equivalence : Equivalence Matches where
  refl a := by cases a <;> constructor
  symm h := by cases h <;> constructor
  trans h₁ h₂ := by cases h₁ <;> cases h₂ <;> constructor

/--
info: 'MoCore.Verdict.matches_equivalence' does not depend on any axioms
-/
#guard_msgs in #print axioms matches_equivalence

/-- A native allocation failure matches exactly the same native allocation failure. -/
theorem native_allocation_matches_only_itself (v : Nat) (b : Signature) :
    Matches (.memory .nativeAllocation v) b ↔ b = .memory .nativeAllocation v := by
  constructor
  · intro h; cases h; rfl
  · intro h; subst h; exact .memory

/--
info: 'MoCore.Verdict.native_allocation_matches_only_itself' does not depend on any axioms
-/
#guard_msgs in #print axioms native_allocation_matches_only_itself

/-- Signatures of different classes never match. -/
theorem matches_same_class {a b : Signature} (h : Matches a b) :
    (∃ r f file r' f' file' s s', a = .defect r f file s ∧ b = .defect r' f' file' s') ∨
    (∃ n v n' v', a = .work n v ∧ b = .work n' v') ∨
    (∃ l v l' v', a = .memory l v ∧ b = .memory l' v') ∨
    (∃ l v l' v', a = .stateSize l v ∧ b = .stateSize l' v') := by
  cases h
  · exact .inl ⟨_, _, _, _, _, _, _, _, rfl, rfl⟩
  · exact .inr (.inl ⟨_, _, _, _, rfl, rfl⟩)
  · exact .inr (.inr (.inl ⟨_, _, _, _, rfl, rfl⟩))
  · exact .inr (.inr (.inr ⟨_, _, _, _, rfl, rfl⟩))

/--
info: 'MoCore.Verdict.matches_same_class' does not depend on any axioms
-/
#guard_msgs in #print axioms matches_same_class

/-! ## The verdict -/

theorem reproduced_iff (recorded : Signature) (o : Outcome) :
    reproduced recorded o = true ↔ Reproduced recorded o := by
  constructor
  · intro h
    cases o with
    | refused => simp [reproduced, signature?] at h
    | stopped s => exact ⟨s, .stopped, (signaturesMatch_iff _ _).1 (by simpa [reproduced, signature?] using h)⟩
    | returned reply changed size =>
      cases size with
      | failed l v =>
        exact ⟨_, .sizeFailed, (signaturesMatch_iff _ _).1 (by simpa [reproduced, signature?] using h)⟩
      | notNeeded | passed => simp [reproduced, signature?] at h
  · rintro ⟨s, hl, hm⟩
    cases hl <;> simpa [reproduced, signature?] using (signaturesMatch_iff _ _).2 hm

/--
info: 'MoCore.Verdict.reproduced_iff' depends on axioms: [propext, Classical.choice, Quot.sound]
-/
#guard_msgs in #print axioms reproduced_iff

theorem passes_iff (r : Outcome) : passes r = true ↔ Succeeded r ∧ SizeOk r := by
  constructor
  · intro h
    cases r with
    | returned reply changed size =>
      cases size with
      | notNeeded => exact ⟨.returned, .notNeeded⟩
      | passed => exact ⟨.returned, .passed⟩
      | failed => simp [passes] at h
    | _ => simp [passes] at h
  · rintro ⟨hs, hz⟩
    cases hs; cases hz <;> rfl

/--
info: 'MoCore.Verdict.passes_iff' depends on axioms: [propext]
-/
#guard_msgs in #print axioms passes_iff

theorem not_reproduced_refused (recorded : Signature) : ¬ Reproduced recorded .refused := by
  rintro ⟨s, hl, _⟩; cases hl

/--
info: 'MoCore.Verdict.not_reproduced_refused' does not depend on any axioms
-/
#guard_msgs in #print axioms not_reproduced_refused

/-- The verdict, with its refusal rows written out. -/
theorem verdict_eq (recorded : Signature) (o r : Outcome) :
    verdict recorded o r =
      if o = .refused then .refused else if r = .refused then .refused
      else if reproduced recorded o then (if passes r then .verified else .notFixed)
      else .unverified := by
  cases o <;> cases r <;> simp [verdict]

/--
info: 'MoCore.Verdict.verdict_eq' depends on axioms: [propext]
-/
#guard_msgs in #print axioms verdict_eq

/-- Soundness and completeness: the verdict is `verified` exactly when the original reproduced
    the recorded failure and the revised run returned with no failed size check. -/
theorem verdict_verified_iff (recorded : Signature) (o r : Outcome) :
    verdict recorded o r = .verified ↔ Reproduced recorded o ∧ Succeeded r ∧ SizeOk r := by
  rw [← reproduced_iff, ← passes_iff, verdict_eq]
  by_cases ho : o = .refused
  · subst ho; simp [reproduced, signature?]
  by_cases hr : r = .refused
  · subst hr; simp [passes]
  simp only [ho, hr, ite_false]
  cases reproduced recorded o <;> cases passes r <;> simp

/--
info: 'MoCore.Verdict.verdict_verified_iff' depends on axioms: [propext, Classical.choice, Quot.sound]
-/
#guard_msgs in #print axioms verdict_verified_iff

theorem verdict_sound : Sound verdict := verdict_verified_iff

/--
info: 'MoCore.Verdict.verdict_sound' depends on axioms: [propext, Classical.choice, Quot.sound]
-/
#guard_msgs in #print axioms verdict_sound

/-- A `verified` verdict is never produced without a reproduced original, whatever the revised
    run did. -/
theorem verdict_not_verified_of_not_reproduced (recorded : Signature) (o : Outcome)
    (h : ¬ Reproduced recorded o) (r : Outcome) : verdict recorded o r ≠ .verified :=
  fun hv => h ((verdict_verified_iff recorded o r).1 hv).1

/--
info: 'MoCore.Verdict.verdict_not_verified_of_not_reproduced' depends on axioms: [propext, Classical.choice, Quot.sound]
-/
#guard_msgs in #print axioms verdict_not_verified_of_not_reproduced

/-- The verdict function is design 7.1's table: each pair of outcomes gets exactly the verdict
    its row gives. -/
theorem verdict_table (recorded : Signature) (o r : Outcome) (v : Verdict) :
    verdict recorded o r = v ↔ Table recorded o r v := by
  have hrep := reproduced_iff recorded o
  have hpass := passes_iff r
  constructor
  · intro h
    subst h
    by_cases ho : o = .refused
    · subst ho; exact .refusedOriginal
    by_cases hr : r = .refused
    · subst hr; cases o <;> first | exact .refusedRevised | exact absurd rfl ho
    have hv : verdict recorded o r =
        if reproduced recorded o then (if passes r then .verified else .notFixed)
        else .unverified := by
      cases o <;> cases r <;> simp_all [verdict]
    rw [hv]
    by_cases h1 : reproduced recorded o = true
    · by_cases h2 : passes r = true
      · simp only [h1, h2, ite_true]
        exact .verified (hrep.1 h1) (hpass.1 h2).1 (hpass.1 h2).2
      · simp only [h1, h2, ite_true]
        exact .notFixed (hrep.1 h1) hr (fun hp => h2 (hpass.2 hp))
    · simp only [h1]
      exact .unverified ho hr (fun hp => h1 (hrep.2 hp))
  · intro h
    cases h with
    | refusedOriginal => rfl
    | refusedRevised => cases o <;> rfl
    | verified hp hs hz => exact (verdict_verified_iff recorded o r).2 ⟨hp, hs, hz⟩
    | notFixed hp hr hn =>
      have h1 := hrep.2 hp
      have h2 : passes r = false := by
        cases hb : passes r
        · rfl
        · exact absurd (hpass.1 hb) hn
      cases o <;> cases r <;> simp_all [verdict, not_reproduced_refused]
    | unverified ho hr hn =>
      have h1 : reproduced recorded o = false := by
        cases hb : reproduced recorded o
        · rfl
        · exact absurd (hrep.1 hb) hn
      cases o <;> cases r <;> simp_all [verdict]

/--
info: 'MoCore.Verdict.verdict_table' depends on axioms: [propext, Classical.choice, Quot.sound]
-/
#guard_msgs in #print axioms verdict_table

/-! ## Negative control

  A verdict that returns `verified` whenever the revised run succeeds, without asking whether
  the original reproduced, must fail the soundness statement `Sound`. The pair below is the
  counterexample: the original returned (nothing reproduced) and so did the revision. -/

def verdictRevisedOnly (recorded : Signature) (o r : Outcome) : Verdict :=
  match o, r with
  | .refused, _ | _, .refused => .refused
  | _, _ =>
    if passes r then .verified else if reproduced recorded o then .notFixed else .unverified

def controlRecorded : Signature := .defect "integer overflow" 3 0 (10, 14)
def controlOriginal : Outcome := .returned .ok true .passed
def controlRevised : Outcome := .returned .ok true .passed

#guard verdictRevisedOnly controlRecorded controlOriginal controlRevised == .verified
#guard verdict controlRecorded controlOriginal controlRevised == .unverified

theorem verdictRevisedOnly_unsound : ¬ Sound verdictRevisedOnly := by
  intro h
  obtain ⟨⟨s, hl, _⟩, _⟩ := (h controlRecorded controlOriginal controlRevised).1 rfl
  cases hl

/--
info: 'MoCore.Verdict.verdictRevisedOnly_unsound' depends on axioms: [propext]
-/
#guard_msgs in #print axioms verdictRevisedOnly_unsound

end MoCore.Verdict
