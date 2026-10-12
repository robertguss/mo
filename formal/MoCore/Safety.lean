/-
  Mo Core: type safety of the calculus.

  `eval_safe`: in a well-typed program, if every primitive respects its
  signature (`PrimsSafe P`), then evaluating a well-typed expression in an
  environment that fits its context never goes `wrong`, returns only values of
  the expression's type, and jumps only to join points in scope with values of
  their parameter types. The proof is by induction on the fuel; each helper of
  the evaluator (`evalArgs`, `applyClosure`, `callFun`, `selectArm`,
  `joinLoop`) has its own lemma, stated for any safe evaluator `ev`.

  `PrimsSafe` is discharged for every primitive in `MoCore/PrimSafety.lean`,
  and the unconditional theorem is `type_safety` in `MoCore/Theorems.lean`.
-/
import MoCore.Typing

namespace MoCore

variable {P : Program}

/-! ## Inversion of value typing -/

theorem ValTy.bool_inv (h : ValTy P v .bool) : ∃ b, v = .bool b := by
  cases h with
  | bool => exact ⟨_, rfl⟩
  | con hcs => simp [ctorsOf] at hcs

theorem Lit.value_ty {l : Lit} (h : l.Ok) : ValTy P l.value l.ty := by
  cases l <;> first
    | exact .int h | exact .decimal h | exact .time h | exact .duration h
    | exact .bigInt | exact .float | exact .bool | exact .char | exact .text | exact .unit

/-! ## Sequencing -/

/-- A non-tail outcome (typed with no join points in scope) followed by a safe
continuation is safe. -/
theorem OutcomeOk.bind {o : Outcome} {k : Value → World → Outcome} {σ τ : Ty} {Δ : JCtx}
    (ho : OutcomeOk P [] σ o) (hk : ∀ v w, ValTy P v σ → OutcomeOk P Δ τ (k v w)) :
    OutcomeOk P Δ τ (o.bind k) := by
  cases o with
  | ok v w => exact hk v w ho
  | stopped => trivial
  | jumped j vs w => obtain ⟨ts, h, _⟩ := ho; simp [lookup] at h
  | wrong => exact absurd ho id
  | outOfFuel => trivial

/-! ## The evaluator's helpers -/

section Helpers

variable {ev : Env → World → Expr → Outcome} (hev : EvSafe P ev)
include hev

/-- Arguments evaluated left to right, then a safe continuation. -/
theorem evalArgs_safe {Γ : Ctx} {ρ : Env} (hρ : EnvTy P ρ Γ) {Δ : JCtx} {τ : Ty} :
    ∀ {es : List Expr} {ts : List Ty} {w : World} {k : List Value → World → Outcome},
    TypedArgs P Γ es ts → (∀ vs w', ValsTy P vs ts → OutcomeOk P Δ τ (k vs w')) →
    OutcomeOk P Δ τ (evalArgs ev ρ w es k)
  | [], _, w, _, h, hk => by cases h; exact hk [] w .nil
  | _ :: _, _, w, _, h, hk => by
    cases h with
    | cons he hes =>
      simp only [evalArgs]
      exact OutcomeOk.bind (hev _ _ _ _ _ w he hρ) fun v w' hv =>
        evalArgs_safe hρ hes fun vs w'' hvs => hk _ _ (.cons hv hvs)

/-- Applying a function value to arguments of its parameter types. -/
theorem applyClosure_safe {fv : Value} {ts : List Ty} {τ : Ty} {vs : List Value} {w : World}
    {Δ : JCtx} (hf : ValTy P fv (.fn ts τ)) (hvs : ValsTy P vs ts) :
    OutcomeOk P Δ τ (applyClosure ev fv vs w) := by
  cases hf with
  | closure hρ hbody =>
    simp only [applyClosure]
    rw [ite_eq_left (by rw [hvs.length_eq, List.length_map])]
    exact (hev _ _ _ _ _ w hbody (EnvTy.bindParams hvs hρ)).weaken
  | con hcs => simp [ctorsOf] at hcs

/-- Calling a top-level function of a well-typed program. -/
theorem callFun_safe (hP : WellTyped P) {f : String} {fd : FunDecl} {vs : List Value} {w : World}
    {Δ : JCtx} (hf : lookupFun P f = some fd) (hvs : ValsTy P vs (fd.params.map Prod.snd)) :
    OutcomeOk P Δ fd.ret (callFun ev P f vs w) := by
  simp only [callFun, hf]
  rw [ite_eq_left (by rw [hvs.length_eq, List.length_map])]
  have hρ : EnvTy P (MoCore.bindParams fd.params vs) fd.params := by
    simpa using EnvTy.bindParams hvs .nil
  exact (hev _ _ _ _ _ w (hP f fd hf) hρ).weaken

omit hev in
/-- Projecting part `i` of a tuple. -/
theorem project_safe {v : Value} {ts : List Ty} {i : Nat} {τ : Ty} {w : World} {Δ : JCtx}
    (hv : ValTy P v (.tuple ts)) (hi : ts[i]? = some τ) : OutcomeOk P Δ τ (project v i w) := by
  cases hv with
  | tuple hvs =>
    obtain ⟨x, hx, hxt⟩ := hvs.getElem? hi
    simp only [project, hx]
    exact hxt
  | con hcs => simp [ctorsOf] at hcs

omit hev in
/-- The arm for a constructor of a well-typed `case`. -/
theorem findArm_typed {Γ : Ctx} {Δ : JCtx} {τ : Ty} :
    ∀ {arms : List (String × List String × Expr)} {cs : List Ctor} {c : String}
      {fs : List (String × Ty)},
    TypedArms P Γ Δ arms cs τ → ctorFields c cs = some fs →
    ∃ xs body, findArm c arms = some (xs, body) ∧ xs.length = fs.length ∧
      Typed P (bindTys xs fs ++ Γ) Δ body τ
  | [], [], _, _, .nil, h => by simp [ctorFields] at h
  | _ :: _, k :: _, c, _, .cons hl hb ha, h => by
    by_cases hc : c = k.name
    · simp only [ctorFields, hc, ite_true, Option.some.injEq] at h
      subst h
      exact ⟨_, _, by simp [findArm, hc], hl, hb⟩
    · simp only [ctorFields, hc, ite_false] at h
      obtain ⟨xs, body, hfind, hl', hb'⟩ := findArm_typed ha h
      exact ⟨xs, body, by simp [findArm, hc, hfind], hl', hb'⟩

/-- Running the arm of a well-typed `case` for a value of the scrutinee's type. -/
theorem selectArm_safe {Γ : Ctx} {Δ : JCtx} {ρ : Env} {arms : List (String × List String × Expr)}
    {cs : List Ctor} {τ σ : Ty} {v : Value} {w : World} (hρ : EnvTy P ρ Γ)
    (harms : TypedArms P Γ Δ arms cs τ) (hcs : ctorsOf P σ = some cs) (hv : ValTy P v σ) :
    OutcomeOk P Δ τ (selectArm ev ρ arms v w) := by
  cases hv with
  | con hcs' hfs hvs =>
    rw [hcs] at hcs'
    cases hcs'
    obtain ⟨xs, body, hfind, hl, hb⟩ := findArm_typed harms hfs
    simp only [selectArm, hfind]
    rw [ite_eq_left (by rw [hl, hvs.length_eq, List.length_map])]
    exact hev _ _ _ _ _ w hb (EnvTy.bindVars hl hvs hρ)
  | _ => simp [ctorsOf] at hcs

/-- A join point's loop: each jump to it runs its body again. -/
theorem joinLoop_safe {Γ : Ctx} {Δ : JCtx} {ρ : Env} {j : String} {ps : List (String × Ty)}
    {body : Expr} {τ : Ty} (hρ : EnvTy P ρ Γ)
    (hbody : Typed P (ps ++ Γ) ((j, ps.map Prod.snd) :: Δ) body τ) :
    ∀ (fuel : Nat) (vs : List Value) (w : World), ValsTy P vs (ps.map Prod.snd) →
    OutcomeOk P Δ τ (joinLoop ev ρ j ps body fuel vs w)
  | 0, _, _, _ => trivial
  | fuel + 1, vs, w, hvs => by
    simp only [joinLoop]
    rw [ite_eq_left (by rw [hvs.length_eq, List.length_map])]
    have h := hev _ _ _ _ _ w hbody (EnvTy.bindParams hvs hρ)
    generalize ev (MoCore.bindParams ps vs ++ ρ) w body = o at h ⊢
    cases o with
    | jumped j' vs' w' =>
      obtain ⟨ts, hts, hvs'⟩ := h
      simp only
      by_cases hj : j' = j
      · subst hj
        simp only [lookup, ite_true, Option.some.injEq] at hts
        subst hts
        simp only [ite_true]
        exact joinLoop_safe hρ hbody fuel vs' w' hvs'
      · simp only [lookup, hj, ite_false] at hts
        simp only [hj, ite_false]
        exact ⟨ts, hts, hvs'⟩
    | ok v w' => exact h
    | stopped => trivial
    | wrong => exact absurd h id
    | outOfFuel => trivial

/-- A `join`: a jump to it from its `rest` runs its body. -/
theorem catchJump_safe {Γ : Ctx} {Δ : JCtx} {ρ : Env} {j : String} {ps : List (String × Ty)}
    {body : Expr} {τ : Ty} {fuel : Nat} {o : Outcome} (hρ : EnvTy P ρ Γ)
    (hbody : Typed P (ps ++ Γ) ((j, ps.map Prod.snd) :: Δ) body τ)
    (hrest : OutcomeOk P ((j, ps.map Prod.snd) :: Δ) τ o) :
    OutcomeOk P Δ τ (catchJump ev ρ j ps body fuel o) := by
  cases o with
  | jumped j' vs w' =>
    obtain ⟨ts, hts, hvs⟩ := hrest
    simp only [catchJump]
    by_cases hj : j' = j
    · subst hj
      simp only [lookup, ite_true, Option.some.injEq] at hts
      subst hts
      simp only [ite_true]
      exact joinLoop_safe hev hρ hbody fuel vs w' hvs
    · simp only [lookup, hj, ite_false] at hts
      simp only [hj, ite_false]
      exact ⟨ts, hts, hvs⟩
  | ok v w' => exact hrest
  | stopped => trivial
  | wrong => exact absurd hrest id
  | outOfFuel => trivial

end Helpers

/-! ## Type safety of the calculus -/

/-- **Type safety, given safe primitives.** In a well-typed program whose
primitives respect their signatures, `eval n P` is safe for every fuel `n`:
a well-typed expression, evaluated in an environment that fits its context,
never goes `wrong`; a value it returns has its type; and a jump it ends with
goes to a join point in scope, with values of its parameter types. -/
theorem eval_safe (hP : WellTyped P) (hprims : PrimsSafe P) : ∀ n, EvSafe P (eval n P)
  | 0 => fun _ _ _ _ _ _ _ _ => trivial
  | n + 1 => by
    have ih := eval_safe hP hprims n
    intro Γ Δ e τ ρ w he hρ
    cases he with
    | lit hl => exact Lit.value_ty hl
    | var hx =>
      obtain ⟨v, hv, hvt⟩ := hρ.lookup hx
      simp only [eval, hv]
      exact hvt
    | let_ he hb =>
      simp only [eval]
      exact (ih _ _ _ _ _ w he hρ).bind fun v w' hv => ih _ _ _ _ _ w' hb (.cons hv hρ)
    | lam hb => exact .closure hρ hb
    | app hf hargs =>
      simp only [eval]
      exact (ih _ _ _ _ _ w hf hρ).bind fun fv w1 hfv =>
        evalArgs_safe ih hρ hargs fun vs w2 hvs => applyClosure_safe ih hfv hvs
    | call hf hargs =>
      simp only [eval]
      exact evalArgs_safe ih hρ hargs fun vs w' hvs => callFun_safe ih hP hf hvs
    | tuple hes =>
      simp only [eval]
      exact evalArgs_safe ih hρ hes fun vs w' hvs => ValTy.tuple hvs
    | proj he hi =>
      simp only [eval]
      exact (ih _ _ _ _ _ w he hρ).bind fun v w' hv => project_safe hv hi
    | con hcs hfs hargs =>
      simp only [eval]
      exact evalArgs_safe ih hρ hargs fun vs w' hvs => ValTy.con hcs hfs hvs
    | case_ he hcs harms =>
      simp only [eval]
      exact (ih _ _ _ _ _ w he hρ).bind fun v w' hv => selectArm_safe ih hρ harms hcs hv
    | if_ hc ht hf =>
      simp only [eval]
      refine (ih _ _ _ _ _ w hc hρ).bind fun v w' hv => ?_
      obtain ⟨b, rfl⟩ := hv.bool_inv
      cases b
      · exact ih _ _ _ _ _ w' hf hρ
      · exact ih _ _ _ _ _ w' ht hρ
    | prim hargs =>
      simp only [eval]
      exact evalArgs_safe ih hρ hargs fun vs w' hvs => (hprims _ vs w' hvs).weaken
    | stop => trivial
    | join hb hr =>
      simp only [eval]
      exact catchJump_safe ih hρ hb (ih _ _ _ _ _ w hr hρ)
    | jump hj hargs =>
      simp only [eval]
      exact evalArgs_safe ih hρ hargs fun vs w' hvs => ⟨_, hj, hvs⟩

end MoCore
