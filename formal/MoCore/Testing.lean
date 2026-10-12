/-
  Mo Core: checking the samples' examples and tests.

  A Mo example line or test (reference 12) lowers to a Core expression of
  type `Bool`. `passes` checks one: it must be well typed (by the checker,
  so `type_safety` applies to it) and evaluate to `true`. Fakes (reference
  12.4) are the world the expression runs in, and capability handles bound to
  the names the test uses.
-/
import MoCore.Check
import MoCore.Lib

namespace MoCore

open DSL

/-- The context of the named capability handles. -/
def capsCtx (caps : List (String × Cap)) : Ctx := caps.map fun (x, c) => (x, .cap c)

/-- An example or test: well typed at `Bool`, with the named capabilities in
scope, and `true` when evaluated in the world. -/
def passes (P : Program) (e : Expr) (w : World := {}) (caps : List (String × Cap) := []) : Bool :=
  typedAs? P (capsCtx caps) e .bool && holds P e w (handles caps)

/-- A test that runs code in steps sharing one world, as a test's fakes are
shared by every call that uses them. Before each step, `set` may change the
world, as a test does when it makes a new fake (such as a new `Args.of(...)`);
every step must be well typed and evaluate to `true`. -/
def scenario (P : Program) (caps : List (String × Cap)) (w : World) :
    List ((World → World) × Expr) → Bool
  | [] => true
  | (set, e) :: steps =>
    typedAs? P (capsCtx caps) e .bool &&
      match run P e (set w) (handles caps) with
      | .ok (.bool true) w' => scenario P caps w' steps
      | _ => false

/-- A fresh `Args.of(list)`. -/
def withArgs (args : List String) (w : World) : World := { w with args := args }

/-- A step that keeps the world as it is. -/
def same (w : World) : World := w

/-! ## From checked programs to theorems

Each sample proves `wellTyped? program = true` with `decide +kernel`: the
kernel itself runs the checker. With the checker's soundness and type safety,
that gives theorems such as "`main` never goes wrong, in any world". -/

/-- Capability handles fit the context of their names. -/
theorem handles_typed {P : Program} : ∀ caps : List (String × Cap), EnvTy P (handles caps) (capsCtx caps)
  | [] => .nil
  | (_, _) :: caps => .cons .cap (handles_typed caps)

/-- What the checker accepts at a type is well typed at it. -/
theorem typedAs_sound {P : Program} {Γ : Ctx} {e : Expr} {τ : Ty} (h : typedAs? P Γ e τ = true) :
    Typed P Γ [] e τ := by
  unfold typedAs? at h
  split at h
  · rename_i τ' hτ'
    exact checkExpr_sound hτ'
  · cases h

/-- An expression the checker accepts, in a program it accepts, never goes
wrong in an environment that fits the expression's context, with any fuel and
in any world. -/
theorem never_wrong {P : Program} {Γ : Ctx} {e : Expr} {τ : Ty} (hP : wellTyped? P = true)
    (he : typedAs? P Γ e τ = true) {ρ : Env} (hρ : EnvTy P ρ Γ) (n : Nat) (w : World) (msg : String) :
    eval n P ρ w e ≠ .wrong msg :=
  eval_never_wrong (checkProgram_sound hP) (typedAs_sound he) hρ n w msg

/-- The same, for an expression run with capability handles, such as a call
of `main`: whatever the world holds (arguments, input, files, time), it never
goes wrong. -/
theorem never_wrong_with_handles {P : Program} {e : Expr} {τ : Ty} {caps : List (String × Cap)}
    (hP : wellTyped? P = true) (he : typedAs? P (capsCtx caps) e τ = true) (n : Nat) (w : World)
    (msg : String) : eval n P (handles caps) w e ≠ .wrong msg :=
  never_wrong hP he (handles_typed caps) n w msg

end MoCore
