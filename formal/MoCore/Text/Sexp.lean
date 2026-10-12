import MoCore.Value

namespace MoCore.Text

inductive Sexp where
  | atom (s : String)
  | str (s : String)
  | node (xs : List Sexp)
  deriving Repr, Inhabited

def hex (n : Nat) (width : Nat := 0) : String :=
  let digits := Nat.repr n
  let rec go : Nat → Nat → List Char
    | 0, _ => []
    | fuel + 1, n =>
      let c := "0123456789abcdef".toList[n % 16]!
      if n < 16 then [c] else go fuel (n / 16) ++ [c]
  let s := String.ofList (go (digits.length + 1) n)
  String.ofList (List.replicate (width - s.length) '0') ++ s

def quote (s : String) : String :=
  "\"" ++ String.join (s.toList.map fun c =>
    match c with
    | '"' => "\\\"" | '\\' => "\\\\" | '\n' => "\\n" | '\r' => "\\r" | '\t' => "\\t"
    | c => if c.toNat < 32 || c.toNat == 127 then "\\u{" ++ hex c.toNat ++ "}"
      else String.singleton c) ++ "\""

partial def Sexp.render : Sexp → String
  | .atom s => s
  | .str s => quote s
  | .node xs => "(" ++ String.intercalate " " (xs.map Sexp.render) ++ ")"

def tag (name : String) (xs : List Sexp := []) : Sexp := .node (.atom name :: xs)
def seq (xs : List Sexp) : Sexp := .node xs
def integer (n : Int) : Sexp := .atom (toString n)
def natural (n : Nat) : Sexp := .atom (toString n)
def boolean (b : Bool) : Sexp := .atom (if b then "true" else "false")

abbrev Parse := Except String

def asString : Sexp → Parse String
  | .str s => .ok s
  | _ => .error "expected a string"

def asInt : Sexp → Parse Int
  | .atom s =>
    let ds := if s.startsWith "-" then s.toList.drop 1 else s.toList
    if !ds.isEmpty && ds.all (fun c => '0' ≤ c && c ≤ '9') then
      match s.toInt? with
      | some n => .ok n
      | none => .error "invalid integer"
    else .error "expected decimal whole number"
  | _ => .error "expected whole number"

def asNat (s : Sexp) : Parse Nat := do
  let n ← asInt s
  if n < 0 then throw "expected nonnegative whole number" else pure n.toNat

def asBool : Sexp → Parse Bool
  | .atom "true" => .ok true
  | .atom "false" => .ok false
  | _ => .error "expected true or false"

def asList (f : Sexp → Parse α) : Sexp → Parse (List α)
  | .node xs => xs.mapM f
  | _ => .error "expected list"

def asPair {α β : Type} (f : Sexp → Parse α) (g : Sexp → Parse β) : Sexp → Parse (α × β)
  | .node [a, b] => return (← f a, ← g b)
  | _ => .error "expected pair"

def unhex (cs : List Char) : Parse Nat := cs.foldlM (fun n c =>
  let d := if '0' ≤ c && c ≤ '9' then some (c.toNat - 48)
    else if 'a' ≤ c && c ≤ 'f' then some (c.toNat - 87)
    else if 'A' ≤ c && c ≤ 'F' then some (c.toNat - 55) else none
  match d with
  | some d => pure (n * 16 + d)
  | none => throw "invalid hexadecimal digit") 0

def asBits : Sexp → Parse UInt64
  | .atom s => do
    if !s.startsWith "0x" || s.length != 18 then throw "expected 0x and 16 hex digits"
    pure (← unhex (s.toList.drop 2)).toUInt64
  | _ => .error "expected Float bits"

def bits (b : UInt64) : Sexp := .atom ("0x" ++ hex b.toNat 16)

def asChar (s : Sexp) : Parse Char := do
  match (← asString s).toList with
  | [c] => pure c
  | _ => throw "expected exactly one Unicode scalar"

def white (c : Char) : Bool := c == ' ' || c == '\t' || c == '\n' || c == '\r'
def boundary (c : Char) : Bool := white c || c == '(' || c == ')' || c == '"' || c == ';'

def skip : List Char → List Char
  | [] => []
  | c :: cs => if white c then skip cs else if c == ';' then comment cs else c :: cs
where
  comment : List Char → List Char
    | [] => []
    | c :: cs => if c == '\n' then skip cs else comment cs

partial def readString : List Char → List Char → Parse (String × List Char)
  | [], _ => .error "unterminated string"
  | '"' :: cs, acc => .ok (String.ofList acc.reverse, cs)
  | '\\' :: cs, acc => match cs with
    | '"' :: rest => readString rest ('"' :: acc)
    | '\\' :: rest => readString rest ('\\' :: acc)
    | 'n' :: rest => readString rest ('\n' :: acc)
    | 'r' :: rest => readString rest ('\r' :: acc)
    | 't' :: rest => readString rest ('\t' :: acc)
    | 'u' :: '{' :: rest => do
      let (ds, tail) := rest.span (· != '}')
      if ds.isEmpty || ds.length > 6 then throw "Unicode escape needs 1 to 6 hex digits"
      let n ← unhex ds
      if n > 0x10ffff || (0xd800 ≤ n && n ≤ 0xdfff) then throw "not a Unicode scalar"
      match tail with
      | '}' :: tail => readString tail (Char.ofNat n :: acc)
      | _ => throw "unterminated Unicode escape"
    | _ => .error "unknown string escape"
  | c :: cs, acc => if c.toNat < 32 || c.toNat == 127 then .error "raw control in string"
    else readString cs (c :: acc)

partial def readOne (input : List Char) : Parse (Sexp × List Char) := do
  match skip input with
  | [] => throw "expected S-expression"
  | '(' :: cs => do
    let (xs, rest) ← readNodes cs
    pure (.node xs, rest)
  | ')' :: _ => throw "unexpected closing parenthesis"
  | '"' :: cs => do
    let (s, rest) ← readString cs []
    pure (.str s, rest)
  | cs =>
    let (a, rest) := cs.span (fun c => !boundary c)
    pure (.atom (String.ofList a), rest)
where
  readNodes (input : List Char) : Parse (List Sexp × List Char) := do
    match skip input with
    | [] => throw "unterminated list"
    | ')' :: cs => pure ([], cs)
    | cs =>
      let (x, rest) ← readOne cs
      let (xs, rest) ← readNodes rest
      pure (x :: xs, rest)

def parseSexp (text : String) : Parse Sexp := do
  let (s, rest) ← readOne text.toList
  if !(skip rest).isEmpty then throw "trailing input"
  pure s

def document (s : Sexp) : String := (tag "core" [.atom "0.4", s]).render ++ "\n"
def parseDocument (text : String) : Parse Sexp := do
  match ← parseSexp text with
  | .node [.atom "core", .atom "0.4", s] => pure s
  | _ => throw "expected (core 0.4 payload)"

end MoCore.Text
