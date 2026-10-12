import MoCore.Syntax

namespace MoCore

/-- Preorder, visiting only Expr constructors, children in text field/list order. -/
partial def exprNodes (e : Expr) : List Expr :=
  e :: (match e with
    | .let_ _ a b | .join _ _ a b => exprNodes a ++ exprNodes b
    | .lam _ b | .proj b _ | .fail b => exprNodes b
    | .app f xs => exprNodes f ++ xs.flatMap exprNodes
    | .call _ xs | .tuple xs | .con _ _ xs | .prim _ xs | .jump _ xs => xs.flatMap exprNodes
    | .case_ s arms => exprNodes s ++ arms.flatMap (fun (_, _, b) => exprNodes b)
    | .if_ c t f => exprNodes c ++ exprNodes t ++ exprNodes f
    | _ => [])

def binderSlots : Expr → Nat
  | .let_ .. => 1
  | .lam ps _ | .join _ ps _ _ => ps.length
  | .case_ _ arms => (arms.map fun (_, ns, _) => ns.length).foldl (· + ·) 0
  | _ => 0

def metadataName (s : String) : Bool := !s.isEmpty && !s.contains '\x00'
def spanValid (files : Nat) (s : Option SourceSpan) : Bool := s.all fun s =>
  s.file < files && s.start ≤ s.end && s.end ≤ 4294967295

def bodyMetaValid (files params : Nat) (e : Expr) (b : BodyMeta) : Bool := Id.run do
  let nodes := (exprNodes e).toArray
  if b.spans.length != nodes.size || b.params.length != params ||
      !b.spans.all (spanValid files) || !b.params.all (fun n => n.all metadataName) then return false
  if !(b.bindings.zip b.bindings.tail).all (fun (a, c) =>
      a.node < c.node || (a.node == c.node && a.slot < c.slot)) then return false
  if !b.bindings.all (fun x => metadataName x.name && match nodes[x.node]? with
      | some e => x.slot < binderSlots e | none => false) then return false
  let lambdas := (nodes.toList.zipIdx).filterMap fun (e, i) =>
    match e with | .lam .. => some i | _ => none
  return b.closures.map Prod.fst == lambdas && b.closures.all (fun (_, n) => metadataName n)

/-- Transport shape only: never certify source truth or feed metadata into a run. -/
def metadataValid (p : Program) : Bool := Id.run do
  let .source files ms fs cs := p.metadata | return true
  if files.length > 4096 || ms.length > 4096 || !files.all metadataName ||
      !ms.all (fun m => metadataName m.name) ||
      !(ms.map (·.name)).Nodup || fs.length != p.funs.length || cs.length != p.consts.length then return false
  let partition := fun (xs : List Nat) (n : Nat) => xs.mergeSort (· ≤ ·) == List.range n
  if !partition (ms.flatMap (·.data)) p.data.length ||
      !partition (ms.flatMap (·.funs)) p.funs.length ||
      !partition (ms.flatMap (·.consts)) p.consts.length then return false
  if !ms.all (fun m =>
      (m.exposedTypes.map Prod.fst).Nodup && m.exposedFuns.Nodup &&
      m.exposedTypes.all (fun (i, _) => m.data.contains i) &&
      m.exposedFuns.all (fun i => m.funs.contains i &&
        (p.funs[i]?.map (·.kind)) == some .function) &&
      m.funs.all (fun i => (p.funs[i]?.map (·.module)) == some m.name)) then return false
  if !(p.consts.zip cs).all (fun (c, m) => metadataName m.name && spanValid files.length m.span &&
      bodyMetaValid files.length 0 c.body m.body) then return false
  return (p.funs.zip fs).all fun (f, m) =>
    metadataName m.name && spanValid files.length m.span &&
    bodyMetaValid files.length f.params.length f.body m.body &&
    (m.text.isSome == f.kind.isSpec) &&
    (m.lines.all fun l => spanValid files.length l.span) &&
    (if f.kind == .promise then !m.lines.isEmpty else m.lines.isEmpty) &&
    (m.allows.isSome == (f.kind == .anchor)) &&
    (match f.kind, m.subject with
      | .promise, some i => (p.funs[i]?.map (·.kind)) == some .function
      | .anchor, some i => (p.funs[i]?.map (·.kind)) == some .promise
      | .promise, none | .anchor, none => false
      | _, none => true
      | _, some _ => false)

end MoCore
