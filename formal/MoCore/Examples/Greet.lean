/-
  examples/greet.mo in Core.

  Exercises: `main` and its capabilities, an `Option` with `or`, and
  interpolation (reference 1, 8.1, 3.4, 11.2).
-/
import MoCore.Testing

namespace MoCore.Examples.Greet

open MoCore DSL Lib

/-- ```
fn greeting(name: Option(Text)) : Text
  "Hello, #{name or "world"}!"
end
``` -/
def greeting : FunDecl :=
  fn "greeting" [("name", tOpt tText)] tText <|
    cat [str "Hello, ", orElse (v "name") (str "world"), str "!"]

/-- ```
fn main(args: Args, console: Console)
  console.print(greeting(args.list.first))
end
``` -/
def mainFn : FunDecl :=
  fn "main" [("args", tCap .args), ("console", tCap .console)] tUnit <|
    print (v "console") (call "greeting" [lop tText .first [argsList (v "args")]])

/-- One example line becomes a zero-argument Unit spec function. -/
def greetingExample (name : String) (input expected : Expr) : FunDecl :=
  { (fn name [] tUnit (assertThat (teq (call "greeting" [input]) expected)
      (str "greeting did not match the expected text"))) with kind := .example }

def program : Program := wholeProgram "Greet" "main" [] []
  [greeting, mainFn,
   greetingExample "greeting.1" (some_ tText (str "Ada")) (str "Hello, Ada!"),
   greetingExample "greeting.2" (none_ tText) (str "Hello, world!")]

/-- The checker accepts the program; `decide +kernel` has the kernel run it. -/
theorem checked : wellTyped? program = true := by decide +kernel

/-- So the program is well typed, and `type_safety` applies to it. -/
theorem wellTyped : WellTyped program := checkProgram_sound checked

/-! ### Examples -/

-- greeting(Some("Ada")) == "Hello, Ada!"
#guard match runTest defaultFuel program "greeting.1" with
  | .ok .unit _ => true
  | _ => false
-- greeting(None) == "Hello, world!"
#guard match runTest defaultFuel program "greeting.2" with
  | .ok .unit _ => true
  | _ => false

/-! ### Test "only the first argument is used"

```
console = Console.fake()
main(Args.of(["Ada", "Grace"]), console)
assert console.printed == ["Hello, Ada!"]
``` -/
#guard passes program
  (seq (call "main" [v "args", v "console"]) <|
    eq (tList tText) (io .printed [v "console"]) (strs ["Hello, Ada!"]))
  { args := ["Ada", "Grace"] } [("args", .args), ("console", .console)]

-- The designated entry uses supplied runtime handles, after initialization.
#guard match runProgram defaultFuel program (handles [("args", .args), ("console", .console)] |>.map Prod.snd)
    { args := ["Ada", "Grace"] } with
  | .ok .unit w => w.stdout == ["Hello, Ada!"]
  | _ => false

/-! ### Never wrong -/

/-- Running `main` never goes wrong, whatever the arguments, with any fuel. -/
theorem main_never_wrong (n : Nat) (w : World) (msg : String) :
    evalProgram n program (handles [("args", .args), ("console", .console)]) w
      (call "main" [v "args", v "console"]) ≠ .wrong msg :=
  never_wrong_with_handles (τ := tUnit) checked (by decide +kernel) n w msg

end MoCore.Examples.Greet
