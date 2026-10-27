import MoCore.Text.Sexp

namespace MoCore.Text

def capTable : List (String × Cap) :=
  [("console", .console), ("files", .files), ("readFiles", .readFiles), ("clock", .clock),
   ("random", .random), ("args", .args), ("env", .env)]

def enumOut [BEq α] (table : List (String × α)) (v : α) : Sexp :=
  tag ((table.find? (fun (_, a) => a == v)).map Prod.fst |>.getD "invalid")

def enumIn (table : List (String × α)) : Sexp → Parse α
  | .node [.atom s] => match lookup s table with
    | some v => pure v
    | none => throw ("unknown constructor: " ++ s)
  | _ => .error "expected nullary constructor"

def tyAtoms : List (String × Ty) :=
  [("int", .int), ("bigInt", .bigInt), ("byte", .byte), ("decimal", .decimal),
   ("float", .float), ("bool", .bool), ("char", .char), ("text", .text), ("bytes", .bytes),
   ("unit", .unit), ("range", .range), ("time", .time), ("duration", .duration),
   ("ordering", .ordering), ("fileError", .fileError), ("entry", .entry), ("entryKind", .entryKind),
   ("simulation", .simulation), ("simulationError", .simulationError)]

partial def tyOut : Ty → Sexp
  | .tuple ts => tag "tuple" [seq (ts.map tyOut)]
  | .list t => tag "list" [tyOut t]
  | .map k v => tag "map" [tyOut k, tyOut v]
  | .set t => tag "set" [tyOut t]
  | .data s => tag "data" [.str s]
  | .option t => tag "option" [tyOut t]
  | .result t e => tag "result" [tyOut t, tyOut e]
  | .fn ps r => tag "fn" [seq (ps.map tyOut), tyOut r]
  | .cap c => tag "cap" [enumOut capTable c]
  | t => enumOut tyAtoms t

partial def tyIn : Sexp → Parse Ty
  | .node [.atom "tuple", ts] => return .tuple (← asList tyIn ts)
  | .node [.atom "list", t] => return .list (← tyIn t)
  | .node [.atom "map", k, v] => return .map (← tyIn k) (← tyIn v)
  | .node [.atom "set", t] => return .set (← tyIn t)
  | .node [.atom "data", s] => return .data (← asString s)
  | .node [.atom "option", t] => return .option (← tyIn t)
  | .node [.atom "result", t, e] => return .result (← tyIn t) (← tyIn e)
  | .node [.atom "fn", ps, r] => return .fn (← asList tyIn ps) (← tyIn r)
  | .node [.atom "cap", c] => return .cap (← enumIn capTable c)
  | s => enumIn tyAtoms s

def litOut : Lit → Sexp
  | .int n => tag "int" [integer n]
  | .bigInt n => tag "bigInt" [integer n]
  | .byte n => tag "byte" [integer n]
  | .decimal n => tag "decimal" [integer n]
  | .float b => tag "float" [bits b]
  | .bool b => tag "bool" [boolean b]
  | .char c => tag "char" [.str (String.singleton c)]
  | .text s => tag "text" [.str s]
  | .bytes bs => tag "bytes" [seq (bs.map integer)]
  | .unit => tag "unit"
  | .time n => tag "time" [integer n]
  | .duration n => tag "duration" [integer n]

def litIn : Sexp → Parse Lit
  | .node [.atom "int", n] => return .int (← asInt n)
  | .node [.atom "bigInt", n] => return .bigInt (← asInt n)
  | .node [.atom "byte", n] => return .byte (← asInt n)
  | .node [.atom "decimal", n] => return .decimal (← asInt n)
  | .node [.atom "float", b] => return .float (← asBits b)
  | .node [.atom "bool", b] => return .bool (← asBool b)
  | .node [.atom "char", c] => return .char (← asChar c)
  | .node [.atom "text", s] => return .text (← asString s)
  | .node [.atom "bytes", bs] => return .bytes (← asList asInt bs)
  | .node [.atom "unit"] => return .unit
  | .node [.atom "time", n] => return .time (← asInt n)
  | .node [.atom "duration", n] => return .duration (← asInt n)
  | _ => .error "invalid literal constructor or arity"

#guard [Ty.int, .map (.data "a\nβ") (.fn [.byte, .bytes] (.set .range)), .cap .readFiles].all fun t =>
  match parseDocument (document (tyOut t)) >>= tyIn with
  | .ok u => t == u && document (tyOut t) == document (tyOut u)
  | _ => false

#guard [Lit.bigInt (-123456789012345678901234567890), .char '😀', .text "a\n\"\\\t", .bytes [0, 255],
  .float 0x8000000000000000, .float 0x7ff8000000000001].all fun l =>
  match parseDocument (document (litOut l)) >>= litIn with
  | .ok m => l == m && document (litOut l) == document (litOut m)
  | _ => false

end MoCore.Text
