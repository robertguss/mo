/-
  Mo Core: an executable type checker, proven sound.

  `tc fuel P Γ Δ e exp` checks `e` against the expected type `exp` (`some τ`)
  or infers its type (`none`). Function parameters, join-point parameters and
  constructor applications carry their types, so most types are found; a
  `stop` or a `jump` takes the type expected of it, and an `if`, `case` or
  `join` infers from the first branch that has a type of its own.

  `tc_sound`: whatever the checker accepts is well typed. So
  `checkProgram_sound` turns a successful `checkProgram P` into `WellTyped P`,
  the hypothesis of `type_safety`.
-/
import MoCore.Theorems

namespace MoCore

/-- Whether a found type is the expected one. -/
def expectTy (exp : Option Ty) (τ : Ty) : Except String Ty :=
  match exp with
  | none => .ok τ
  | some τ' => if τ' = τ then .ok τ else .error s!"expected {τ'}, found {τ}"

/-- Checks arguments against parameter types. -/
def tcArgs (tcE : Expr → Option Ty → Except String Ty) : List Expr → List Ty → Except String Unit
  | [], [] => .ok ()
  | e :: es, t :: ts =>
    match tcE e (some t) with
    | .ok _ => tcArgs tcE es ts
    | .error m => .error m
  | _, _ => .error "the wrong number of arguments"

/-- Infers the types of a tuple's parts. -/
def tcAll (tcE : Expr → Option Ty → Except String Ty) : List Expr → Except String (List Ty)
  | [] => .ok []
  | e :: es =>
    match tcE e none with
    | .ok t =>
      match tcAll tcE es with
      | .ok ts => .ok (t :: ts)
      | .error m => .error m
    | .error m => .error m

/-- Checks a `case`'s arms: one per constructor, in order, each binding its
constructor's fields; all of one type. -/
def tcArms (tcE : Ctx → Expr → Option Ty → Except String Ty) (Γ : Ctx) :
    List (String × List String × Expr) → List Ctor → Option Ty → Except String Ty
  | [], [], some τ => .ok τ
  | [], [], none => .error "can't infer the type of a case without arms"
  | (c, xs, body) :: arms, k :: ks, exp =>
    if c = k.name ∧ xs.length = k.fields.length then
      match exp with
      | some τ =>
        match tcE (bindTys xs k.fields ++ Γ) body (some τ) with
        | .ok _ => tcArms tcE Γ arms ks (some τ)
        | .error m => .error s!"in the arm {c}: {m}"
      | none =>
        match tcE (bindTys xs k.fields ++ Γ) body none with
        | .ok τ => tcArms tcE Γ arms ks (some τ)
        | .error _ =>
          match tcArms tcE Γ arms ks none with
          | .ok τ =>
            match tcE (bindTys xs k.fields ++ Γ) body (some τ) with
            | .ok _ => .ok τ
            | .error m => .error s!"in the arm {c}: {m}"
          | .error m => .error m
    else .error s!"the arm {c} doesn't match the constructor {k.name}"
  | _, _, _ => .error "a case needs one arm per constructor"

/-- The type checker. -/
def tc : Nat → Program → Ctx → JCtx → Expr → Option Ty → Except String Ty
  | 0, _, _, _, _, _ => .error "the expression is too deep to check"
  | n + 1, P, Γ, Δ, e, exp =>
    match e with
    | .lit l => if l.Ok then expectTy exp l.ty else .error "a number literal out of range"
    | .var x =>
      match lookup x Γ with
      | some τ => expectTy exp τ
      | none => .error s!"unbound variable {x}"
    | .let_ x e body =>
      match tc n P Γ [] e none with
      | .ok σ => tc n P ((x, σ) :: Γ) Δ body exp
      | .error m => .error s!"in {x} = ...: {m}"
    | .lam ps body =>
      match exp with
      | some (.fn ts r) =>
        if ts = ps.map Prod.snd then
          match tc n P (ps ++ Γ) [] body (some r) with
          | .ok _ => .ok (.fn ts r)
          | .error m => .error m
        else .error "a function's parameters don't have the expected types"
      | some τ => .error s!"a function where a {τ} is expected"
      | none =>
        match tc n P (ps ++ Γ) [] body none with
        | .ok r => .ok (.fn (ps.map Prod.snd) r)
        | .error m => .error m
    | .app f args =>
      match tc n P Γ [] f none with
      | .ok (.fn ts τ) =>
        match tcArgs (tc n P Γ []) args ts with
        | .ok _ => expectTy exp τ
        | .error m => .error m
      | .ok τ => .error s!"applying a {τ}"
      | .error m => .error m
    | .call f args =>
      match lookupFun P f with
      | some fd =>
        match tcArgs (tc n P Γ []) args (fd.params.map Prod.snd) with
        | .ok _ => expectTy exp fd.ret
        | .error m => .error s!"in a call of {f}: {m}"
      | none => .error s!"no function {f}"
    | .tuple es =>
      match tcAll (tc n P Γ []) es with
      | .ok ts => expectTy exp (.tuple ts)
      | .error m => .error m
    | .proj e i =>
      match tc n P Γ [] e none with
      | .ok (.tuple ts) =>
        match ts[i]? with
        | some τ => expectTy exp τ
        | none => .error s!"a tuple without a part {i}"
      | .ok τ => .error s!"projecting from a {τ}"
      | .error m => .error m
    | .con τ c args =>
      match ctorsOf P τ with
      | some cs =>
        match ctorFields c cs with
        | some fs =>
          match tcArgs (tc n P Γ []) args (fs.map Prod.snd) with
          | .ok _ => expectTy exp τ
          | .error m => .error s!"in {c}(...): {m}"
        | none => .error s!"{τ} has no constructor {c}"
      | none => .error s!"{τ} has no constructors"
    | .case_ e arms =>
      match tc n P Γ [] e none with
      | .ok σ =>
        match ctorsOf P σ with
        | some cs => tcArms (fun Γ' b ex => tc n P Γ' Δ b ex) Γ arms cs exp
        | none => .error s!"case on a {σ}"
      | .error m => .error m
    | .if_ c t f =>
      match tc n P Γ [] c (some .bool) with
      | .ok _ =>
        match exp with
        | some τ =>
          match tc n P Γ Δ t (some τ) with
          | .ok _ => tc n P Γ Δ f (some τ)
          | .error m => .error m
        | none =>
          match tc n P Γ Δ t none with
          | .ok τ => tc n P Γ Δ f (some τ)
          | .error _ =>
            match tc n P Γ Δ f none with
            | .ok τ =>
              match tc n P Γ Δ t (some τ) with
              | .ok _ => .ok τ
              | .error m => .error m
            | .error m => .error m
      | .error m => .error s!"in a condition: {m}"
    | .prim p args =>
      match tcArgs (tc n P Γ []) args p.sig.1 with
      | .ok _ => expectTy exp p.sig.2
      | .error m => .error s!"in a primitive: {m}"
    | .stop _ =>
      match exp with
      | some τ => .ok τ
      | none => .error "can't infer the type of a stop"
    | .fail detail =>
      match tc n P Γ [] detail (some .text) with
      | .ok _ =>
        match exp with
        | some τ => .ok τ
        | none => .error "can't infer the type of a fail"
      | .error m => .error s!"in a failure detail: {m}"
    | .join j ps body rest =>
      match exp with
      | some τ =>
        match tc n P (ps ++ Γ) ((j, ps.map Prod.snd) :: Δ) body (some τ) with
        | .ok _ => tc n P Γ ((j, ps.map Prod.snd) :: Δ) rest (some τ)
        | .error m => .error s!"in the join point {j}: {m}"
      | none =>
        match tc n P Γ ((j, ps.map Prod.snd) :: Δ) rest none with
        | .ok τ =>
          match tc n P (ps ++ Γ) ((j, ps.map Prod.snd) :: Δ) body (some τ) with
          | .ok _ => .ok τ
          | .error m => .error s!"in the join point {j}: {m}"
        | .error _ =>
          match tc n P (ps ++ Γ) ((j, ps.map Prod.snd) :: Δ) body none with
          | .ok τ => tc n P Γ ((j, ps.map Prod.snd) :: Δ) rest (some τ)
          | .error m => .error s!"in the join point {j}: {m}"
    | .jump j args =>
      match lookup j Δ with
      | some ts =>
        match tcArgs (tc n P Γ []) args ts with
        | .ok _ =>
          match exp with
          | some τ => .ok τ
          | none => .error "can't infer the type of a jump"
        | .error m => .error s!"in a jump to {j}: {m}"
      | none => .error s!"a jump to {j}, which isn't in scope here (or not in tail position)"

/-! ## Soundness -/

/-- A checker is sound when what it accepts is well typed, at the expected
type if one was given. -/
def TcSound (P : Program) (f : Ctx → JCtx → Expr → Option Ty → Except String Ty) : Prop :=
  ∀ Γ Δ e exp τ, f Γ Δ e exp = .ok τ → Typed P Γ Δ e τ ∧ ∀ τ', exp = some τ' → τ' = τ

theorem expectTy_ok {exp : Option Ty} {τ τ' : Ty} (h : expectTy exp τ = .ok τ') :
    τ' = τ ∧ ∀ τ'', exp = some τ'' → τ'' = τ := by
  cases exp with
  | none => simp only [expectTy, Except.ok.injEq] at h; exact ⟨h.symm, by simp⟩
  | some τ'' =>
    simp only [expectTy] at h
    split at h
    · simp only [Except.ok.injEq] at h; subst_vars; exact ⟨rfl, by simp⟩
    · cases h

theorem tcArgs_sound {P : Program} {Γ : Ctx} {tcE : Expr → Option Ty → Except String Ty}
    (hE : ∀ e exp τ, tcE e exp = .ok τ → Typed P Γ [] e τ ∧ ∀ τ', exp = some τ' → τ' = τ) :
    ∀ {es : List Expr} {ts : List Ty} {u : Unit}, tcArgs tcE es ts = .ok u → TypedArgs P Γ es ts
  | [], [], _, _ => .nil
  | e :: es, t :: ts, _, h => by
    simp only [tcArgs] at h
    split at h
    · rename_i τ hτ
      obtain ⟨he, hexp⟩ := hE _ _ _ hτ
      obtain rfl := hexp t rfl
      exact .cons he (tcArgs_sound hE h)
    · cases h
  | [], _ :: _, _, h => by simp [tcArgs] at h
  | _ :: _, [], _, h => by simp [tcArgs] at h

theorem tcAll_sound {P : Program} {Γ : Ctx} {tcE : Expr → Option Ty → Except String Ty}
    (hE : ∀ e exp τ, tcE e exp = .ok τ → Typed P Γ [] e τ ∧ ∀ τ', exp = some τ' → τ' = τ) :
    ∀ {es : List Expr} {ts : List Ty}, tcAll tcE es = .ok ts → TypedArgs P Γ es ts
  | [], ts, h => by simp only [tcAll, Except.ok.injEq] at h; subst h; exact .nil
  | e :: es, ts, h => by
    simp only [tcAll] at h
    split at h
    · rename_i t ht
      split at h
      · rename_i ts' hts'
        simp only [Except.ok.injEq] at h
        subst h
        exact .cons (hE _ _ _ ht).1 (tcAll_sound hE hts')
      · cases h
    · cases h

theorem tcArms_sound {P : Program} {Δ : JCtx} {tcE : Ctx → Expr → Option Ty → Except String Ty}
    (hE : ∀ Γ' e exp τ, tcE Γ' e exp = .ok τ → Typed P Γ' Δ e τ ∧ ∀ τ', exp = some τ' → τ' = τ)
    {Γ : Ctx} :
    ∀ {arms : List (String × List String × Expr)} {cs : List Ctor} {exp : Option Ty} {τ : Ty},
    tcArms tcE Γ arms cs exp = .ok τ → TypedArms P Γ Δ arms cs τ ∧ ∀ τ', exp = some τ' → τ' = τ
  | [], [], some τ', τ, h => by
    simp only [tcArms, Except.ok.injEq] at h; subst h; exact ⟨.nil, by simp⟩
  | [], [], none, _, h => by simp [tcArms] at h
  | (c, xs, body) :: arms, k :: ks, exp, τ, h => by
    simp only [tcArms] at h
    split at h
    · rename_i hck
      obtain ⟨rfl, hlen⟩ := hck
      cases exp with
      | some τ' =>
        simp only at h
        split at h
        · rename_i τb hb
          obtain ⟨hbt, hbe⟩ := hE _ _ _ _ hb
          obtain rfl := hbe τ' rfl
          obtain ⟨harms, hexp⟩ := tcArms_sound hE h
          obtain rfl := hexp _ rfl
          exact ⟨.cons hlen hbt harms, by simp⟩
        · cases h
      | none =>
        simp only at h
        split at h
        · rename_i τb hb
          obtain ⟨hbt, _⟩ := hE _ _ _ _ hb
          obtain ⟨harms, hexp⟩ := tcArms_sound hE h
          obtain rfl := hexp _ rfl
          exact ⟨.cons hlen hbt harms, by simp⟩
        · split at h
          · rename_i τa ha
            split at h
            · rename_i τb hb
              simp only [Except.ok.injEq] at h
              subst h
              obtain ⟨hbt, hbe⟩ := hE _ _ _ _ hb
              obtain rfl := hbe _ rfl
              exact ⟨.cons hlen hbt (tcArms_sound hE ha).1, by simp⟩
            · cases h
          · cases h
    · cases h
  | [], _ :: _, _, _, h => by simp [tcArms] at h
  | _ :: _, [], _, _, h => by simp [tcArms] at h

/-- **The checker is sound**: for every fuel, what `tc` accepts is well typed. -/
theorem tc_sound {P : Program} : ∀ n, TcSound P (tc n P)
  | 0 => by intro Γ Δ e exp τ h; simp [tc] at h
  | n + 1 => by
    have ih := tc_sound (P := P) n
    intro Γ Δ e exp τ h
    cases e with
    | lit l =>
      simp only [tc] at h
      split at h
      · obtain ⟨rfl, hexp⟩ := expectTy_ok h
        exact ⟨.lit ‹_›, hexp⟩
      · cases h
    | var x =>
      simp only [tc] at h
      split at h
      · obtain ⟨rfl, hexp⟩ := expectTy_ok h
        exact ⟨.var ‹_›, hexp⟩
      · cases h
    | let_ x e body =>
      simp only [tc] at h
      split at h
      · rename_i σ hσ
        obtain ⟨hb, hexp⟩ := ih _ _ _ _ _ h
        exact ⟨.let_ (ih _ _ _ _ _ hσ).1 hb, hexp⟩
      · cases h
    | lam ps body =>
      simp only [tc] at h
      split at h
      · rename_i ts r
        split at h
        · rename_i hts
          split at h
          · rename_i r' hr
            simp only [Except.ok.injEq] at h
            subst h hts
            obtain ⟨hb, hr'⟩ := ih _ _ _ _ _ hr
            obtain rfl := hr' r rfl
            exact ⟨.lam hb, by simp⟩
          · cases h
        · cases h
      · cases h
      · split at h
        · rename_i r hr
          simp only [Except.ok.injEq] at h
          subst h
          exact ⟨.lam (ih _ _ _ _ _ hr).1, by simp⟩
        · cases h
    | app f args =>
      simp only [tc] at h
      split at h
      · rename_i ts τf hf
        split at h
        · rename_i hargs
          obtain ⟨rfl, hexp⟩ := expectTy_ok h
          exact ⟨.app (ih _ _ _ _ _ hf).1 (tcArgs_sound (fun e ex t he => ih _ _ e ex t he) hargs), hexp⟩
        · cases h
      · cases h
      · cases h
    | call f args =>
      simp only [tc] at h
      split at h
      · rename_i fd hfd
        split at h
        · rename_i hargs
          obtain ⟨rfl, hexp⟩ := expectTy_ok h
          exact ⟨.call hfd (tcArgs_sound (fun e ex t he => ih _ _ e ex t he) hargs), hexp⟩
        · cases h
      · cases h
    | tuple es =>
      simp only [tc] at h
      split at h
      · rename_i ts hts
        obtain ⟨rfl, hexp⟩ := expectTy_ok h
        exact ⟨.tuple (tcAll_sound (fun e ex t he => ih _ _ e ex t he) hts), hexp⟩
      · cases h
    | proj e i =>
      simp only [tc] at h
      split at h
      · rename_i ts hts
        split at h
        · rename_i τi hi
          obtain ⟨rfl, hexp⟩ := expectTy_ok h
          exact ⟨.proj (ih _ _ _ _ _ hts).1 hi, hexp⟩
        · cases h
      · cases h
      · cases h
    | con τc c args =>
      simp only [tc] at h
      split at h
      · rename_i cs hcs
        split at h
        · rename_i fs hfs
          split at h
          · rename_i hargs
            obtain ⟨rfl, hexp⟩ := expectTy_ok h
            exact ⟨.con hcs hfs (tcArgs_sound (fun e ex t he => ih _ _ e ex t he) hargs), hexp⟩
          · cases h
        · cases h
      · cases h
    | case_ e arms =>
      simp only [tc] at h
      split at h
      · rename_i σ hσ
        split at h
        · rename_i cs hcs
          obtain ⟨harms, hexp⟩ :=
            tcArms_sound (Δ := Δ) (fun Γ' b ex t hb => ih Γ' Δ b ex t hb) h
          exact ⟨.case_ (ih _ _ _ _ _ hσ).1 hcs harms, hexp⟩
        · cases h
      · cases h
    | if_ c t f =>
      simp only [tc] at h
      split at h
      · rename_i hc
        have hct := (ih _ _ _ _ _ hc).1
        obtain rfl := (ih _ _ _ _ _ hc).2 _ rfl
        cases exp with
        | some τ' =>
          simp only at h
          split at h
          · rename_i ht
            obtain ⟨htt, hte⟩ := ih _ _ _ _ _ ht
            obtain ⟨hft, hfe⟩ := ih _ _ _ _ _ h
            obtain rfl := hfe _ rfl
            obtain rfl := hte _ rfl
            exact ⟨.if_ hct htt hft, by simp⟩
          · cases h
        | none =>
          simp only at h
          split at h
          · rename_i τt ht
            obtain ⟨hft, hfe⟩ := ih _ _ _ _ _ h
            obtain rfl := hfe _ rfl
            exact ⟨.if_ hct (ih _ _ _ _ _ ht).1 hft, by simp⟩
          · split at h
            · rename_i τf hf
              split at h
              · rename_i ht
                simp only [Except.ok.injEq] at h
                subst h
                obtain ⟨htt, hte⟩ := ih _ _ _ _ _ ht
                obtain rfl := hte _ rfl
                exact ⟨.if_ hct htt (ih _ _ _ _ _ hf).1, by simp⟩
              · cases h
            · cases h
      · cases h
    | prim p args =>
      simp only [tc] at h
      split at h
      · rename_i hargs
        obtain ⟨rfl, hexp⟩ := expectTy_ok h
        exact ⟨.prim (tcArgs_sound (fun e ex t he => ih _ _ e ex t he) hargs), hexp⟩
      · cases h
    | stop r =>
      simp only [tc] at h
      split at h
      · simp only [Except.ok.injEq] at h
        subst h
        exact ⟨.stop, by simp_all⟩
      · cases h
    | fail detail =>
      simp only [tc] at h
      split at h
      · rename_i hd
        split at h
        · simp only [Except.ok.injEq] at h
          subst h
          obtain ⟨hdt, hde⟩ := ih _ _ _ _ _ hd
          obtain rfl := hde .text rfl
          exact ⟨.fail hdt, by simp_all⟩
        · cases h
      · cases h
    | join j ps body rest =>
      simp only [tc] at h
      cases exp with
      | some τ' =>
        simp only at h
        split at h
        · rename_i hb
          obtain ⟨hbt, hbe⟩ := ih _ _ _ _ _ hb
          obtain rfl := hbe _ rfl
          obtain ⟨hrt, hre⟩ := ih _ _ _ _ _ h
          obtain rfl := hre _ rfl
          exact ⟨.join hbt hrt, by simp⟩
        · cases h
      | none =>
        simp only at h
        split at h
        · rename_i τr hr
          split at h
          · rename_i hb
            simp only [Except.ok.injEq] at h
            subst h
            obtain ⟨hbt, hbe⟩ := ih _ _ _ _ _ hb
            obtain rfl := hbe _ rfl
            exact ⟨.join hbt (ih _ _ _ _ _ hr).1, by simp⟩
          · cases h
        · split at h
          · rename_i τb hb
            obtain ⟨hrt, hre⟩ := ih _ _ _ _ _ h
            obtain rfl := hre _ rfl
            exact ⟨.join (ih _ _ _ _ _ hb).1 hrt, by simp⟩
          · cases h
    | jump j args =>
      simp only [tc] at h
      split at h
      · rename_i ts hts
        split at h
        · rename_i hargs
          split at h
          · simp only [Except.ok.injEq] at h
            subst h
            exact ⟨.jump hts (tcArgs_sound (fun e ex t he => ih _ _ e ex t he) hargs), by simp_all⟩
          · cases h
        · cases h
      · cases h

/-! ## Checking programs -/

/-- Fuel for the checker: the depth of the deepest expression it can check. -/
def checkFuel : Nat := 100000

/-- Checks an expression against a type, with no join points in scope. -/
def checkExpr (P : Program) (Γ : Ctx) (e : Expr) (τ : Ty) : Except String Ty :=
  tc checkFuel P Γ [] e (some τ)

theorem checkExpr_sound {P : Program} {Γ : Ctx} {e : Expr} {τ τ' : Ty}
    (h : checkExpr P Γ e τ = .ok τ') : Typed P Γ [] e τ := by
  obtain ⟨he, hτ⟩ := tc_sound checkFuel Γ [] e (some τ) τ' h
  obtain rfl := hτ τ rfl
  exact he

/-- Checks every function of a list. -/
def checkFuns (P : Program) : List FunDecl → Except String Unit
  | [] => .ok ()
  | fd :: fds =>
    match tc checkFuel P fd.params [] fd.body (some fd.ret) with
    | .ok _ => checkFuns P fds
    | .error m => .error s!"{fd.name}: {m}"

/-- Checks every function of a program. -/
def checkProgram (P : Program) : Except String Unit := checkFuns P P.funs

/-- Whether a program passes the checker. -/
def wellTyped? (P : Program) : Bool :=
  match checkProgram P with
  | .ok _ => true
  | .error _ => false

/-- Whether an expression passes the checker at a type. -/
def typedAs? (P : Program) (Γ : Ctx) (e : Expr) (τ : Ty) : Bool :=
  match checkExpr P Γ e τ with
  | .ok _ => true
  | .error _ => false

theorem lookupFun_go_mem {f : String} : ∀ {fds : List FunDecl} {fd : FunDecl},
    lookupFun.go f fds = some fd → fd ∈ fds
  | [], _, h => by simp [lookupFun.go] at h
  | g :: gs, fd, h => by
    simp only [lookupFun.go] at h
    split at h
    · simp only [Option.some.injEq] at h; subst h; simp
    · exact List.mem_cons_of_mem _ (lookupFun_go_mem h)

theorem checkFuns_sound {P : Program} : ∀ {fds : List FunDecl} {u : Unit},
    checkFuns P fds = .ok u → ∀ fd ∈ fds, Typed P fd.params [] fd.body fd.ret
  | [], _, _ => by simp
  | g :: gs, _, h => by
    simp only [checkFuns] at h
    split at h
    · rename_i τ hτ
      intro fd hfd
      simp only [List.mem_cons] at hfd
      rcases hfd with rfl | hfd
      · obtain ⟨he, hexp⟩ := tc_sound checkFuel _ _ _ _ _ hτ
        obtain rfl := hexp _ rfl
        exact he
      · exact checkFuns_sound h fd hfd
    · cases h

/-- **A program the checker accepts is well typed**, so `type_safety` applies. -/
theorem checkProgram_sound {P : Program} (h : wellTyped? P = true) : WellTyped P := by
  unfold wellTyped? at h
  split at h
  · rename_i u hu
    intro f fd hfd
    exact checkFuns_sound hu fd (lookupFun_go_mem hfd)
  · cases h

/-- **A checked expression of a checked program never goes wrong**: the
checker and type safety together. -/
theorem checked_never_wrong {P : Program} (hP : wellTyped? P = true) {Γ : Ctx} {e : Expr}
    {τ : Ty} (he : typedAs? P Γ e τ = true) {ρ : Env} (hρ : EnvTy P ρ Γ) (n : Nat) (w : World) :
    OutcomeOk P [] τ (eval n P ρ w e) := by
  unfold typedAs? at he
  split at he
  · rename_i τ' hτ'
    exact type_safety (checkProgram_sound hP) (checkExpr_sound hτ') hρ n w
  · cases he

end MoCore
