/-
  Mo Core: fuel is only a bound.

  `eval_fuel_mono`: if evaluation finishes with fuel `n` (its outcome is not
  `outOfFuel`), then evaluation with any larger fuel has the same outcome. So
  an answer the Lean evaluator gives is the answer: a differential test of the
  VM against the model never depends on how much fuel the model was given.
-/
import MoCore.Eval

namespace MoCore

/-- `ev'` agrees with `ev` wherever `ev` finishes. -/
def Agrees (ev ev' : Env → World → Expr → Outcome) : Prop :=
  ∀ ρ w e, ev ρ w e ≠ .outOfFuel → ev' ρ w e = ev ρ w e

theorem bind_agrees {o o' : Outcome} {k k' : Value → World → Outcome}
    (ho : o ≠ .outOfFuel → o' = o) (hk : ∀ v w, k v w ≠ .outOfFuel → k' v w = k v w)
    (h : o.bind k ≠ .outOfFuel) : o'.bind k' = o.bind k := by
  cases o with
  | ok v w =>
    rw [ho (by simp)]
    exact hk v w h
  | stopped b w => rw [ho (by simp)]; rfl
  | failed detail w => rw [ho (by simp)]; rfl
  | jumped j vs w => rw [ho (by simp)]; rfl
  | wrong m => rw [ho (by simp)]; rfl
  | outOfFuel => exact absurd rfl h

section Helpers

variable {ev ev' : Env → World → Expr → Outcome} (hev : Agrees ev ev')
include hev

theorem evalArgs_agrees {ρ : Env} :
    ∀ {es : List Expr} {w : World} {k k' : List Value → World → Outcome},
    (∀ vs w, k vs w ≠ .outOfFuel → k' vs w = k vs w) →
    evalArgs ev ρ w es k ≠ .outOfFuel → evalArgs ev' ρ w es k' = evalArgs ev ρ w es k
  | [], _, _, _, hk, h => hk _ _ h
  | e :: es, w, k, k', hk, h => by
    simp only [evalArgs] at h ⊢
    exact bind_agrees (hev ρ w e)
      (fun v w' h' => evalArgs_agrees (fun vs w'' h'' => hk _ _ h'') h') h

theorem applyClosure_agrees {fv : Value} {vs : List Value} {w : World}
    (h : applyClosure ev fv vs w ≠ .outOfFuel) : applyClosure ev' fv vs w = applyClosure ev fv vs w := by
  cases fv with
  | closure ρc ps body =>
    simp only [applyClosure] at h ⊢
    split
    · rename_i hl
      simp only [hl, ite_true] at h
      exact hev _ _ _ h
    · rfl
  | _ => rfl

theorem callFun_agrees {P : Program} {f : String} {vs : List Value} {w : World}
    (h : callFun ev P f vs w ≠ .outOfFuel) : callFun ev' P f vs w = callFun ev P f vs w := by
  simp only [callFun] at h ⊢
  split
  · rename_i fd hfd
    simp only [hfd] at h
    split
    · rename_i hl
      simp only [hl, ite_true] at h
      exact hev _ _ _ h
    · rfl
  · rfl

theorem selectArm_agrees {ρ : Env} {arms : List (String × List String × Expr)} {v : Value} {w : World}
    (h : selectArm ev ρ arms v w ≠ .outOfFuel) : selectArm ev' ρ arms v w = selectArm ev ρ arms v w := by
  cases v with
  | con c vs =>
    simp only [selectArm] at h ⊢
    split
    · rename_i xs body hfind
      simp only [hfind] at h
      split
      · rename_i hl
        simp only [hl, ite_true] at h
        exact hev _ _ _ h
      · rfl
    · rfl
  | _ => rfl

/-- The loop of a join point, with one more unit of its own fuel and the
evaluator for one more unit of fuel, agrees wherever it finishes. -/
theorem joinLoop_agrees {ρ : Env} {j : String} {ps : List (String × Ty)} {body : Expr} :
    ∀ (k : Nat) (vs : List Value) (w : World), joinLoop ev ρ j ps body k vs w ≠ .outOfFuel →
    joinLoop ev' ρ j ps body (k + 1) vs w = joinLoop ev ρ j ps body k vs w
  | 0, _, _, h => absurd rfl h
  | k + 1, vs, w, h => by
    simp only [joinLoop] at h ⊢
    split
    · rename_i hl
      simp only [hl, ite_true] at h
      have hb : ev (bindParams ps vs ++ ρ) w body ≠ .outOfFuel := by
        intro hb; rw [hb] at h; exact h rfl
      rw [hev _ _ _ hb]
      generalize ev (bindParams ps vs ++ ρ) w body = o at h ⊢
      cases o with
      | jumped j' vs' w' =>
        simp only at h ⊢
        split
        · rename_i hj
          simp only [hj, ite_true] at h
          exact joinLoop_agrees k vs' w' h
        · rfl
      | _ => rfl
    · rfl

theorem catchJump_agrees {ρ : Env} {j : String} {ps : List (String × Ty)} {body : Expr} {k : Nat}
    {o o' : Outcome} (ho : o ≠ .outOfFuel → o' = o) (h : catchJump ev ρ j ps body k o ≠ .outOfFuel) :
    catchJump ev' ρ j ps body (k + 1) o' = catchJump ev ρ j ps body k o := by
  have hne : o ≠ .outOfFuel := by intro hx; subst hx; exact h rfl
  rw [ho hne]
  cases o with
  | jumped j' vs w' =>
    simp only [catchJump] at h ⊢
    split
    · rename_i hj
      simp only [hj, ite_true] at h
      exact joinLoop_agrees hev k vs w' h
    · rfl
  | _ => rfl

end Helpers

/-- One more unit of fuel never changes an outcome that isn't `outOfFuel`. -/
theorem eval_agrees_succ {P : Program} : ∀ n, Agrees (eval n P) (eval (n + 1) P)
  | 0 => fun _ _ _ h => absurd rfl h
  | n + 1 => by
    have ih := eval_agrees_succ (P := P) n
    intro ρ w e h
    cases e with
    | lit l => rfl
    | var x => rfl
    | lam ps body => rfl
    | stop r => rfl
    | let_ x e body =>
      simp only [eval] at h ⊢
      exact bind_agrees (ih ρ w e) (fun v w' h' => ih _ _ body h') h
    | app f args =>
      simp only [eval] at h ⊢
      exact bind_agrees (ih ρ w f)
        (fun fv w1 h1 => evalArgs_agrees ih (fun vs w2 h2 => applyClosure_agrees ih h2) h1) h
    | call f args =>
      simp only [eval] at h ⊢
      exact evalArgs_agrees ih (fun vs w' h' => callFun_agrees ih h') h
    | tuple es =>
      simp only [eval] at h ⊢
      exact evalArgs_agrees ih (fun _ _ _ => rfl) h
    | proj e i =>
      simp only [eval] at h ⊢
      exact bind_agrees (ih ρ w e) (fun _ _ _ => rfl) h
    | con τ c args =>
      simp only [eval] at h ⊢
      exact evalArgs_agrees ih (fun _ _ _ => rfl) h
    | case_ e arms =>
      simp only [eval] at h ⊢
      exact bind_agrees (ih ρ w e) (fun v w' h' => selectArm_agrees ih h') h
    | if_ c t f =>
      simp only [eval] at h ⊢
      refine bind_agrees (ih ρ w c) (fun v w' h' => ?_) h
      cases v with
      | bool b =>
        cases b
        · exact ih _ _ _ h'
        · exact ih _ _ _ h'
      | _ => rfl
    | prim p args =>
      simp only [eval] at h ⊢
      exact evalArgs_agrees ih (fun _ _ _ => rfl) h
    | fail detail =>
      simp only [eval] at h ⊢
      exact bind_agrees (ih ρ w detail) (fun _ _ _ => rfl) h
    | join j ps body rest =>
      simp only [eval] at h ⊢
      exact catchJump_agrees ih (ih ρ w rest) h
    | jump j args =>
      simp only [eval] at h ⊢
      exact evalArgs_agrees ih (fun _ _ _ => rfl) h

/-- **Fuel is only a bound.** If evaluating with fuel `n` finishes (it doesn't
run out of fuel), evaluating with any fuel `m ≥ n` gives the same outcome. -/
theorem eval_fuel_mono {P : Program} {ρ : Env} {w : World} {e : Expr} {n m : Nat}
    (h : eval n P ρ w e ≠ .outOfFuel) (hm : n ≤ m) : eval m P ρ w e = eval n P ρ w e := by
  induction hm with
  | refl => rfl
  | step _ ih => rw [eval_agrees_succ _ _ _ _ (by rw [ih]; exact h), ih]

end MoCore
