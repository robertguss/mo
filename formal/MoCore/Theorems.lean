/-
  Mo Core: the main theorems, in one place.

  * `type_safety`: a well-typed expression of a well-typed program, evaluated
    in an environment that fits its context, has an outcome that is
    `OutcomeOk`: never `wrong`, a returned value of its type, no stray jump.
  * `eval_never_wrong`, `eval_ok_typed`, `eval_no_stray_jump`: the three parts.
  * `int_result_in_range`: an `Int` result is a 64-bit whole number, so
    overflow can only stop the program, never wrap; likewise
    `byte_result_in_range` and `decimal_result_in_range`.
  * `set_result_distinct`: a `Set` result holds no element twice.
  * `eval_deterministic`: the evaluator is a function.

  All of them hold for every fuel and every world.
-/
import MoCore.PrimSafety

namespace MoCore

/-- **Type safety.** In a well-typed program, a well-typed expression (with
no join points in scope), evaluated with any fuel in any world, in an
environment whose values have the context's types, never goes `wrong`; if it
returns a value, the value has the expression's type; and it never ends with a
jump. (It may also stop on a bug, or run out of fuel.) -/
theorem type_safety {P : Program} (hP : WellTyped P) {Γ : Ctx} {e : Expr} {τ : Ty} {ρ : Env}
    (he : Typed P Γ [] e τ) (hρ : EnvTy P ρ Γ) (n : Nat) (w : World) :
    OutcomeOk P [] τ (eval n P ρ w e) :=
  eval_safe hP prims_safe n Γ [] e τ ρ w he hρ

/-- A well-typed expression never goes wrong. -/
theorem eval_never_wrong {P : Program} (hP : WellTyped P) {Γ : Ctx} {e : Expr} {τ : Ty}
    {ρ : Env} (he : Typed P Γ [] e τ) (hρ : EnvTy P ρ Γ) (n : Nat) (w : World) (msg : String) :
    eval n P ρ w e ≠ .wrong msg := by
  intro h
  have := type_safety hP he hρ n w
  rw [h] at this
  exact this

/-- A value a well-typed expression returns has the expression's type. -/
theorem eval_ok_typed {P : Program} (hP : WellTyped P) {Γ : Ctx} {e : Expr} {τ : Ty}
    {ρ : Env} (he : Typed P Γ [] e τ) (hρ : EnvTy P ρ Γ) {n : Nat} {w w' : World} {v : Value}
    (h : eval n P ρ w e = .ok v w') : ValTy P v τ := by
  have := type_safety hP he hρ n w
  rw [h] at this
  exact this

/-- A well-typed expression never ends with a jump: every jump is caught by
the join point it goes to. -/
theorem eval_no_stray_jump {P : Program} (hP : WellTyped P) {Γ : Ctx} {e : Expr} {τ : Ty}
    {ρ : Env} (he : Typed P Γ [] e τ) (hρ : EnvTy P ρ Γ) (n : Nat) (w w' : World) (j : String)
    (vs : List Value) : eval n P ρ w e ≠ .jumped j vs w' := by
  intro h
  have := type_safety hP he hρ n w
  rw [h] at this
  obtain ⟨_, hj, _⟩ := this
  simp [lookup] at hj

/-- An `Int` that a well-typed expression returns is a 64-bit whole number:
overflow stops the program (reference 13.2), it never wraps. -/
theorem int_result_in_range {P : Program} (hP : WellTyped P) {Γ : Ctx} {e : Expr} {ρ : Env}
    (he : Typed P Γ [] e .int) (hρ : EnvTy P ρ Γ) {n : Nat} {w w' : World} {v : Value}
    (h : eval n P ρ w e = .ok v w') : ∃ k, v = .int k ∧ int64Min ≤ k ∧ k ≤ int64Max := by
  have hv := eval_ok_typed hP he hρ h
  cases hv with
  | int hk => exact ⟨_, rfl, hk⟩
  | con hcs => simp [ctorsOf] at hcs

/-- A `Byte` that a well-typed expression returns is a whole number from 0 to
255. -/
theorem byte_result_in_range {P : Program} (hP : WellTyped P) {Γ : Ctx} {e : Expr} {ρ : Env}
    (he : Typed P Γ [] e .byte) (hρ : EnvTy P ρ Γ) {n : Nat} {w w' : World} {v : Value}
    (h : eval n P ρ w e = .ok v w') : ∃ k, v = .byte k ∧ 0 ≤ k ∧ k ≤ 255 := by
  have hv := eval_ok_typed hP he hρ h
  cases hv with
  | byte hk => exact ⟨_, rfl, hk⟩
  | con hcs => simp [ctorsOf] at hcs

/-- A `Decimal` that a well-typed expression returns is within `±(2^127 - 1)`
counts of `10^-18`. -/
theorem decimal_result_in_range {P : Program} (hP : WellTyped P) {Γ : Ctx} {e : Expr} {ρ : Env}
    (he : Typed P Γ [] e .decimal) (hρ : EnvTy P ρ Γ) {n : Nat} {w w' : World} {v : Value}
    (h : eval n P ρ w e = .ok v w') : ∃ k, v = .decimal k ∧ -decMax ≤ k ∧ k ≤ decMax := by
  have hv := eval_ok_typed hP he hρ h
  cases hv with
  | decimal hk => exact ⟨_, rfl, hk⟩
  | con hcs => simp [ctorsOf] at hcs

/-- A `Set` that a well-typed expression returns holds no element twice: no
element equals (`==`) one after it. -/
theorem set_result_distinct {P : Program} (hP : WellTyped P) {Γ : Ctx} {e : Expr} {ρ : Env}
    {t : Ty} (he : Typed P Γ [] e (.set t)) (hρ : EnvTy P ρ Γ) {n : Nat} {w w' : World}
    {v : Value} (h : eval n P ρ w e = .ok v w') :
    ∃ xs, v = .set xs ∧ xs.Pairwise fun a b => Value.equiv a b = false := by
  have hv := eval_ok_typed hP he hρ h
  cases hv with
  | set _ hd => exact ⟨_, rfl, hd⟩
  | con hcs => simp [ctorsOf] at hcs

/-- **Determinism.** The evaluator is a function, so the same fuel, program,
environment, world and expression always give the same outcome. -/
theorem eval_deterministic {n : Nat} {P : Program} {ρ : Env} {w : World} {e : Expr}
    {o₁ o₂ : Outcome} (h₁ : eval n P ρ w e = o₁) (h₂ : eval n P ρ w e = o₂) : o₁ = o₂ :=
  h₁ ▸ h₂

end MoCore
