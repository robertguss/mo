/-
  Mo Core: typing.

  * `Typed P Γ Δ e τ`: in program `P`, with variable context `Γ` and join-point
    context `Δ`, expression `e` has type `τ`. Jumps are checked against `Δ`,
    and `Δ` is emptied in every position that isn't a tail position (a `let`'s
    bound expression, a condition, a scrutinee, an argument, a function body),
    so jumps can only be in tail position.
  * `WellTyped P`: every function's body has its declared result type.
  * `ValTy P v τ`: value `v` has type `τ`. It includes the number ranges, so
    `Int` values are 64-bit, `Byte` values from 0 to 255 and `Decimal` values
    within `±(2^127 - 1)`, and a set holds no element twice.
  * `OutcomeOk P Δ τ o`: what type safety promises of an outcome.
  * `PrimSafe P p`: primitive `p` respects its signature.
-/
import MoCore.Eval

namespace MoCore

/-- A variable context: names and types, innermost first. -/
abbrev Ctx := List (String × Ty)

/-- A join-point context: the join points in scope and their parameter types. -/
abbrev JCtx := List (String × List Ty)

/-- A case arm's variables, typed by its constructor's fields. -/
def bindTys (xs : List String) (fs : List (String × Ty)) : Ctx :=
  List.zipWith (fun x f => (x, f.2)) xs fs

mutual
/-- The typing judgment for expressions. -/
inductive Typing (P : Program) (scope : Nat) (spec : Bool) : Ctx → JCtx → Expr → Ty → Prop
  | lit {Γ Δ l} : l.Ok → Typing P scope spec Γ Δ (.lit l) l.ty
  | var {Γ Δ x τ} : lookup x Γ = some τ → Typing P scope spec Γ Δ (.var x) τ
  | global {Γ Δ x τ} : lookup x (constCtx P scope) = some τ →
      Typing P scope spec Γ Δ (.global x) τ
  | let_ {Γ Δ x e body σ τ} :
      Typing P scope spec Γ [] e σ → Typing P scope spec ((x, σ) :: Γ) Δ body τ →
      Typing P scope spec Γ Δ (.let_ x e body) τ
  | lam {Γ Δ ps body τ} :
      Typing P scope spec (ps ++ Γ) [] body τ →
      Typing P scope spec Γ Δ (.lam ps body) (.fn (ps.map Prod.snd) τ)
  | app {Γ Δ f args ts τ} :
      Typing P scope spec Γ [] f (.fn ts τ) → TypingArgs P scope spec Γ args ts →
      Typing P scope spec Γ Δ (.app f args) τ
  | call {Γ Δ f fd args} :
      lookupFun P f = some fd → fd.constCount ≤ scope →
      (fd.kind.isSpec = true → spec = true) →
      TypingArgs P scope spec Γ args (fd.params.map Prod.snd) →
      Typing P scope spec Γ Δ (.call f args) fd.ret
  | tuple {Γ Δ es ts} : TypingArgs P scope spec Γ es ts → Typing P scope spec Γ Δ (.tuple es) (.tuple ts)
  | proj {Γ Δ e ts i τ} :
      Typing P scope spec Γ [] e (.tuple ts) → ts[i]? = some τ → Typing P scope spec Γ Δ (.proj e i) τ
  | con {Γ Δ τ cs c fs args} :
      ctorsOf P τ = some cs → ctorFields c cs = some fs →
      TypingArgs P scope spec Γ args (fs.map Prod.snd) → Typing P scope spec Γ Δ (.con τ c args) τ
  | case_ {Γ Δ e σ cs arms τ} :
      Typing P scope spec Γ [] e σ → ctorsOf P σ = some cs → TypingArms P scope spec Γ Δ arms cs τ →
      Typing P scope spec Γ Δ (.case_ e arms) τ
  | if_ {Γ Δ c t f τ} :
      Typing P scope spec Γ [] c .bool → Typing P scope spec Γ Δ t τ →
      Typing P scope spec Γ Δ f τ → Typing P scope spec Γ Δ (.if_ c t f) τ
  | prim {Γ Δ p args} : (p.specOnly = true → spec = true) →
      TypingArgs P scope spec Γ args p.sig.1 → Typing P scope spec Γ Δ (.prim p args) p.sig.2
  /-- `stop` fits wherever a value is expected (reference 8.4). -/
  | stop {Γ Δ r τ} : Typing P scope spec Γ Δ (.stop r) τ
  /-- Failure never returns, but its detail must be Text. -/
  | fail {Γ Δ detail τ} : spec = true → Typing P scope spec Γ [] detail .text →
      Typing P scope spec Γ Δ (.fail detail) τ
  /-- The body sees the parameters and may jump to the join point itself; both
  the body and the rest are in tail position. -/
  | join {Γ Δ j ps body rest τ} :
      Typing P scope spec (ps ++ Γ) ((j, ps.map Prod.snd) :: Δ) body τ →
      Typing P scope spec Γ ((j, ps.map Prod.snd) :: Δ) rest τ →
      Typing P scope spec Γ Δ (.join j ps body rest) τ
  /-- A jump never returns, so it fits any type, like `stop`. -/
  | jump {Γ Δ j ts args τ} :
      lookup j Δ = some ts → TypingArgs P scope spec Γ args ts → Typing P scope spec Γ Δ (.jump j args) τ

/-- Arguments, each typed in a non-tail position. -/
inductive TypingArgs (P : Program) (scope : Nat) (spec : Bool) : Ctx → List Expr → List Ty → Prop
  | nil {Γ} : TypingArgs P scope spec Γ [] []
  | cons {Γ e es t ts} : Typing P scope spec Γ [] e t → TypingArgs P scope spec Γ es ts →
      TypingArgs P scope spec Γ (e :: es) (t :: ts)

/-- One arm per constructor, in declaration order, each binding its
constructor's fields. -/
inductive TypingArms (P : Program) (scope : Nat) (spec : Bool) :
    Ctx → JCtx → List (String × List String × Expr) → List Ctor → Ty → Prop
  | nil {Γ Δ τ} : TypingArms P scope spec Γ Δ [] [] τ
  | cons {Γ Δ xs k body arms ks τ} :
      xs.length = k.fields.length → Typing P scope spec (bindTys xs k.fields ++ Γ) Δ body τ →
      TypingArms P scope spec Γ Δ arms ks τ → TypingArms P scope spec Γ Δ ((k.name, xs, body) :: arms) (k :: ks) τ
end

/-- Expressions default to ordinary code with no constants available. -/
abbrev Typed (P : Program) (Γ : Ctx) (Δ : JCtx) (e : Expr) (τ : Ty)
    (scope : Nat := 0) (spec : Bool := false) := Typing P scope spec Γ Δ e τ
abbrev TypedArgs (P : Program) (Γ : Ctx) (es : List Expr) (ts : List Ty)
    (scope : Nat := 0) (spec : Bool := false) := TypingArgs P scope spec Γ es ts
abbrev TypedArms (P : Program) (Γ : Ctx) (Δ : JCtx)
    (arms : List (String × List String × Expr)) (cs : List Ctor) (τ : Ty)
    (scope : Nat := 0) (spec : Bool := false) := TypingArms P scope spec Γ Δ arms cs τ

/-- Constants see exactly the prefix before them. -/
inductive TypedConsts (P : Program) : Nat → List ConstDecl → Prop
  | nil {i} : TypedConsts P i []
  | cons {i c cs} : Typed P [] [] c.body c.ty i false →
      TypedConsts P (i + 1) cs → TypedConsts P i (c :: cs)

/-- All bodies, kind signatures, prefixes and the designated entry are checked. -/
structure WellTyped (P : Program) : Prop where
  funs : ∀ f fd, lookupFun P f = some fd →
    Typed P fd.params [] fd.body fd.ret fd.constCount fd.kind.isSpec
  consts : TypedConsts P 0 P.consts
  signatures : ∀ fd ∈ P.funs, fd.kind.validSig fd.params fd.ret = true
  scopes : ∀ fd ∈ P.funs, fd.constCount ≤ P.consts.length
  names : (P.consts.map ConstDecl.name).Nodup
  entry : ∃ fd, lookupFun P P.entry = some fd ∧ fd.kind = .function

mutual
/-- Value typing. -/
inductive ValTy (P : Program) : Value → Ty → Prop
  | int {n} : InInt64 n → ValTy P (.int n) .int
  | bigInt {n} : ValTy P (.bigInt n) .bigInt
  | byte {n} : InByte n → ValTy P (.byte n) .byte
  | decimal {n} : InDecimal n → ValTy P (.decimal n) .decimal
  | float {b} : ValTy P (.float b) .float
  | bool {b} : ValTy P (.bool b) .bool
  | char {c} : ValTy P (.char c) .char
  | text {s} : ValTy P (.text s) .text
  | bytes {bs} : (∀ b ∈ bs, InByte b) → ValTy P (.bytes bs) .bytes
  | unit : ValTy P .unit .unit
  | range {a b} : InInt64 a → InInt64 b → ValTy P (.range a b) .range
  | time {t} : InTime t → ValTy P (.time t) .time
  | duration {d} : InInt64 d → ValTy P (.duration d) .duration
  | tuple {vs ts} : ValsTy P vs ts → ValTy P (.tuple vs) (.tuple ts)
  | list {vs t} : (∀ x ∈ vs, ValTy P x t) → ValTy P (.list vs) (.list t)
  | map {es k t} : (∀ e ∈ es, ValTy P e.1 k) → (∀ e ∈ es, ValTy P e.2 t) → ValTy P (.map es) (.map k t)
  /-- A set's elements have its element type, and none equals another. -/
  | set {vs t} : (∀ x ∈ vs, ValTy P x t) → Distinct vs → ValTy P (.set vs) (.set t)
  | con {τ cs c fs vs} :
      ctorsOf P τ = some cs → ctorFields c cs = some fs → ValsTy P vs (fs.map Prod.snd) →
      ValTy P (.con c vs) τ
  /-- A closure has a function type when its captured environment fits a
  context in which its body has the result type. -/
  | closure {ρ Γ ps body τ globals scope spec} :
      EnvTy P ρ Γ → EnvTy P globals (constCtx P scope) → Typed P (ps ++ Γ) [] body τ scope spec →
      ValTy P (.closure ρ ps body globals) (.fn (ps.map Prod.snd) τ)
  | cap {c r fake} : ValTy P (.cap c r fake) (.cap c)

/-- Pointwise value typing. -/
inductive ValsTy (P : Program) : List Value → List Ty → Prop
  | nil : ValsTy P [] []
  | cons {v vs t ts} : ValTy P v t → ValsTy P vs ts → ValsTy P (v :: vs) (t :: ts)

/-- An environment that fits a context: same names, typed values. -/
inductive EnvTy (P : Program) : Env → Ctx → Prop
  | nil : EnvTy P [] []
  | cons {x v t ρ Γ} : ValTy P v t → EnvTy P ρ Γ → EnvTy P ((x, v) :: ρ) ((x, t) :: Γ)
end

/-- What type safety promises of an outcome in join context `Δ` at type `τ`:
a value of type `τ`; or a stop; or running out of fuel; or a jump to a join
point of `Δ` with values of its parameter types. Never `wrong`. -/
def OutcomeOk (P : Program) (Δ : JCtx) (τ : Ty) : Outcome → Prop
  | .ok v _ => ValTy P v τ
  | .stopped _ _ => True
  | .failed _ _ => True
  | .jumped j vs _ => ∃ ts, lookup j Δ = some ts ∧ ValsTy P vs ts
  | .wrong _ => False
  | .outOfFuel => True

/-- A primitive respects its signature: given arguments of its argument types,
it returns a value of its result type, or stops. -/
def PrimSafe (P : Program) (p : Prim) : Prop :=
  ∀ vs w, ValsTy P vs p.sig.1 → OutcomeOk P [] p.sig.2 (Prim.eval p vs w)

/-- Every primitive respects its signature. -/
def PrimsSafe (P : Program) : Prop := ∀ p, PrimSafe P p

/-- An evaluator (such as `eval n P`) is safe when every well-typed expression,
in an environment that fits its context, has an outcome that is `OutcomeOk`. -/
def EvSafe (P : Program) (ev : Env → Env → World → Expr → Outcome) : Prop :=
  ∀ scope spec globals Γ Δ e τ ρ w, Typed P Γ Δ e τ scope spec →
    EnvTy P ρ Γ → EnvTy P globals (constCtx P scope) → OutcomeOk P Δ τ (ev globals ρ w e)

/-! ## Basic facts -/

@[simp] theorem outcomeOk_ok : OutcomeOk P Δ τ (.ok v w) ↔ ValTy P v τ := Iff.rfl
@[simp] theorem outcomeOk_stopped : OutcomeOk P Δ τ (.stopped b w) := trivial
@[simp] theorem outcomeOk_failed : OutcomeOk P Δ τ (.failed detail w) := trivial
@[simp] theorem outcomeOk_outOfFuel : OutcomeOk P Δ τ .outOfFuel := trivial
@[simp] theorem outcomeOk_wrong : ¬ OutcomeOk P Δ τ (.wrong m) := id

/-- An outcome with no jumps is fine in any join context. -/
theorem OutcomeOk.weaken {o : Outcome} (h : OutcomeOk P [] τ o) : OutcomeOk P Δ τ o := by
  cases o with
  | jumped j vs w => obtain ⟨ts, hts, _⟩ := h; simp [lookup] at hts
  | _ => exact h

theorem ValsTy.length_eq : ValsTy P vs ts → vs.length = ts.length
  | .nil => rfl
  | .cons _ h => by simp [h.length_eq]

theorem ValsTy.getElem? : ∀ {vs ts} {i : Nat} {τ}, ValsTy P vs ts → ts[i]? = some τ →
    ∃ v, vs[i]? = some v ∧ ValTy P v τ
  | _, _, 0, _, .cons hv _, h => by simp at h; subst h; exact ⟨_, rfl, hv⟩
  | _, _, i + 1, _, .cons _ hs, h => by simpa using hs.getElem? (by simpa using h)
  | _, _, _, _, .nil, h => by simp at h

theorem ValsTy.replicate {vs : List Value} {t : Ty} (h : ∀ x ∈ vs, ValTy P x t) :
    ValsTy P vs (List.replicate vs.length t) := by
  induction vs with
  | nil => exact .nil
  | cons x xs ih =>
    exact .cons (h x (by simp)) (ih fun y hy => h y (by simp [hy]))

theorem ValsTy.of_replicate : ∀ {vs : List Value} {n : Nat} {t : Ty},
    ValsTy P vs (List.replicate n t) → ∀ x ∈ vs, ValTy P x t
  | [], _, _, _ => by simp
  | _ :: _, 0, _, h => by cases h
  | _ :: _, _ + 1, _, .cons hv hs => by
    intro x hx
    simp only [List.mem_cons] at hx
    rcases hx with rfl | hx
    · exact hv
    · exact ValsTy.of_replicate hs x hx

/-- Looking up a variable of the context in a fitting environment. -/
theorem EnvTy.lookup : ∀ {ρ Γ x τ}, EnvTy P ρ Γ → lookup x Γ = some τ →
    ∃ v, MoCore.lookup x ρ = some v ∧ ValTy P v τ
  | _, _, _, _, .nil, h => by simp [MoCore.lookup] at h
  | _, _, x, _, .cons (x := y) hv hρ, h => by
    by_cases hxy : x = y
    · subst hxy; simp [MoCore.lookup] at h ⊢; subst h; exact hv
    · simp [MoCore.lookup, hxy] at h ⊢; exact hρ.lookup h

/-- Keeping a prefix of an environment keeps the same prefix of its context. -/
theorem EnvTy.take : ∀ {ρ Γ}, EnvTy P ρ Γ → (n : Nat) → EnvTy P (ρ.take n) (Γ.take n)
  | _, _, .nil, _ => by simp; exact .nil
  | _, _, .cons _ _, 0 => .nil
  | _, _, .cons hv hs, n + 1 => .cons hv (hs.take n)

theorem EnvTy.constPrefix {globals : Env} {scope n : Nat}
    (h : EnvTy P globals (constCtx P scope)) (hn : n ≤ scope) :
    EnvTy P (globals.take n) (constCtx P n) := by
  simpa [constCtx, List.map_take, List.take_take, Nat.min_eq_left hn] using h.take n

/-- Appending constants keeps their computation order. -/
theorem EnvTy.append : ∀ {ρ σ Γ ctx}, EnvTy P ρ Γ → EnvTy P σ ctx →
    EnvTy P (ρ ++ σ) (Γ ++ ctx)
  | _, _, _, _, .nil, hs => hs
  | _, _, _, _, .cons hv h, hs => .cons hv (h.append hs)

/-- Binding parameters to values of their types extends a fitting environment. -/
theorem EnvTy.bindParams : ∀ {ps : List (String × Ty)} {vs ρ Γ},
    ValsTy P vs (ps.map Prod.snd) → EnvTy P ρ Γ → EnvTy P (MoCore.bindParams ps vs ++ ρ) (ps ++ Γ)
  | [], _, _, _, .nil, hρ => by simpa [MoCore.bindParams] using hρ
  | (x, t) :: ps, _ :: _, _, _, .cons hv hs, hρ => by
    simp only [MoCore.bindParams, List.zipWith_cons_cons, List.cons_append]
    exact .cons hv (EnvTy.bindParams hs hρ)

/-- Binding a case arm's variables to a constructed value's fields. -/
theorem EnvTy.bindVars : ∀ {xs : List String} {fs : List (String × Ty)} {vs ρ Γ},
    xs.length = fs.length → ValsTy P vs (fs.map Prod.snd) → EnvTy P ρ Γ →
    EnvTy P (MoCore.bindVars xs vs ++ ρ) (bindTys xs fs ++ Γ)
  | [], [], _, _, _, _, .nil, hρ => by simpa [MoCore.bindVars, bindTys] using hρ
  | x :: xs, (_, t) :: fs, _ :: _, _, _, hl, .cons hv hs, hρ => by
    simp only [MoCore.bindVars, bindTys, List.zipWith_cons_cons, List.cons_append]
    exact .cons hv (EnvTy.bindVars (by simpa using hl) hs hρ)
  | [], _ :: _, _, _, _, hl, _, _ => by simp at hl
  | _ :: _, [], _, _, _, hl, _, _ => by simp at hl

end MoCore
