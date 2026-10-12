/-
  Mo Core: checking the samples' examples and tests.

  A Mo example line or test (reference 12) lowers to a Core assertion of
  type `Unit`. `passes` checks its condition and runs the assertion: it must
  be well typed and return normally, rather than fail. Tests construct their
  own fake handles; the optional world and handles exercise real capabilities.
-/
import MoCore.Check
import MoCore.Lib

namespace MoCore

open DSL

/-- The context of the named capability handles. -/
def capsCtx (caps : List (String × Cap)) : Ctx := caps.map fun (x, c) => (x, .cap c)

/-- An example or test condition lowered to a Unit assertion in spec code. -/
def passes (P : Program) (e : Expr) (w : World := {}) (caps : List (String × Cap) := []) : Bool :=
  let assertion := assertThat e (str "sample assertion failed")
  typedAs? P (capsCtx caps) assertion .unit true &&
    match run P assertion w (handles caps) with
    | .ok .unit _ => true
    | _ => false

/-- An expression that must stop on the bug `b`: well typed at `τ`, with the
named capabilities in scope, and stopping with `b` when evaluated in the
world. Mo can't catch a bug (reference 13.2), so no sample's test says this;
the model checks with it that a library call or a map literal stops where the
reference says it does. -/
def stops (P : Program) (e : Expr) (τ : Ty) (b : Bug) (w : World := {})
    (caps : List (String × Cap) := []) : Bool :=
  typedAs? P (capsCtx caps) e τ true &&
    match run P e w (handles caps) with
    | .stopped b' _ => b' == b
    | _ => false

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
    Typed P Γ [] e τ P.consts.length := by
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
    evalProgram n P ρ w e ≠ .wrong msg := by
  intro h
  have hs := program_type_safety (checkProgram_sound hP) (typedAs_sound he) hρ n w
  rw [h] at hs
  exact hs

/-- The same, for an expression run with capability handles, such as a call
of `main`: whatever the world holds (arguments, input, files, time), it never
goes wrong. -/
theorem never_wrong_with_handles {P : Program} {e : Expr} {τ : Ty} {caps : List (String × Cap)}
    (hP : wellTyped? P = true) (he : typedAs? P (capsCtx caps) e τ = true) (n : Nat) (w : World)
    (msg : String) : evalProgram n P (handles caps) w e ≠ .wrong msg :=
  never_wrong hP he (handles_typed caps) n w msg

end MoCore
