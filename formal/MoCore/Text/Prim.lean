import MoCore.Text.Basic

namespace MoCore.Text

-- Tables enumerate the nullary Lean constructors, not their Mo spellings.
def intOps : List (String × IntOp) :=
  [("add", .add), ("sub", .sub), ("mul", .mul), ("div", .div), ("rem", .rem),
   ("neg", .neg), ("abs", .abs), ("min", .min), ("max", .max), ("pow", .pow),
   ("floorDiv", .floorDiv), ("floorMod", .floorMod), ("checkedAdd", .checkedAdd),
   ("checkedSub", .checkedSub), ("checkedMul", .checkedMul), ("wrappingAdd", .wrappingAdd),
   ("wrappingSub", .wrappingSub), ("wrappingMul", .wrappingMul), ("saturatingAdd", .saturatingAdd),
   ("saturatingSub", .saturatingSub), ("saturatingMul", .saturatingMul), ("toText", .toText),
   ("toDecimal", .toDecimal), ("toBig", .toBig), ("toFloat", .toFloat), ("toChar", .toChar),
   ("toByte", .toByte), ("seconds", .seconds), ("milliseconds", .milliseconds),
   ("minutes", .minutes), ("hours", .hours), ("days", .days)]
def bigOps : List (String × BigOp) :=
  [("add", .add), ("sub", .sub), ("mul", .mul), ("div", .div), ("rem", .rem),
   ("neg", .neg), ("abs", .abs), ("min", .min), ("max", .max), ("pow", .pow),
   ("floorDiv", .floorDiv), ("floorMod", .floorMod), ("toInt", .toInt), ("toText", .toText)]
def byteOps : List (String × ByteOp) := [("toInt", .toInt), ("toText", .toText)]
def decOps : List (String × DecOp) :=
  [("add", .add), ("sub", .sub), ("mul", .mul), ("div", .div), ("rem", .rem), ("neg", .neg),
   ("abs", .abs), ("min", .min), ("max", .max), ("floor", .floor), ("ceil", .ceil),
   ("round", .round), ("toInt", .toInt), ("toText", .toText), ("toFixed", .toFixed), ("toFloat", .toFloat)]
def floatOps : List (String × FloatOp) :=
  [("add", .add), ("sub", .sub), ("mul", .mul), ("div", .div), ("rem", .rem), ("neg", .neg), ("abs", .abs),
   ("sqrt", .sqrt), ("floor", .floor), ("ceil", .ceil), ("round", .round),
   ("isNan", .isNan), ("isInfinite", .isInfinite), ("toDecimal", .toDecimal), ("toText", .toText)]
def bases : List (String × Base) :=
  [("int", .int), ("bigInt", .bigInt), ("byte", .byte), ("decimal", .decimal),
   ("float", .float), ("bool", .bool), ("char", .char), ("text", .text),
   ("bytes", .bytes), ("time", .time), ("duration", .duration)]
def cmpOps : List (String × CmpOp) := [("lt", .lt), ("le", .le), ("gt", .gt), ("ge", .ge)]
def charOps : List (String × CharOp) :=
  [("code", .code), ("isDigit", .isDigit), ("isLetter", .isLetter), ("isWhiteSpace", .isWhiteSpace),
   ("lower", .lower), ("upper", .upper), ("toText", .toText)]
def textOps : List (String × TextOp) :=
  [("concat", .concat), ("byteCount", .byteCount), ("charCount", .charCount), ("isEmpty", .isEmpty),
   ("chars", .chars), ("fromChars", .fromChars), ("lines", .lines), ("words", .words),
   ("split", .split), ("trim", .trim), ("trimStart", .trimStart), ("trimEnd", .trimEnd),
   ("startsWith", .startsWith), ("endsWith", .endsWith), ("contains", .contains),
   ("dropPrefix", .dropPrefix), ("dropSuffix", .dropSuffix), ("replace", .replace),
   ("repeat", .«repeat»), ("padLeft", .padLeft), ("padRight", .padRight), ("lower", .lower),
   ("upper", .upper), ("toInt", .toInt), ("toDecimal", .toDecimal), ("toFloat", .toFloat),
   ("join", .join), ("bytes", .bytes)]
def bytesOps : List (String × BytesOp) := [("size", .size), ("get", .get), ("decodeUtf8", .decodeUtf8)]
def unicodeOps : List (String × UnicodeOp) :=
  [("graphemes", .graphemes), ("displayWidth", .displayWidth), ("normalizeNfc", .normalizeNfc),
   ("lowercase", .lowercase), ("uppercase", .uppercase), ("letter", .letter), ("digit", .digit)]
def rangeOps : List (String × RangeOp) :=
  [("make", .make), ("start", .start), ("end", .«end»), ("toList", .toList),
   ("contains", .contains), ("toText", .toText)]
def timeOps : List (String × TimeOp) :=
  [("sub", .sub), ("add", .add), ("subDur", .subDur), ("checkedAdd", .checkedAdd),
   ("toText", .toText), ("parse", .parse), ("durAdd", .durAdd), ("durSub", .durSub),
   ("durMul", .durMul), ("durNeg", .durNeg), ("inMilliseconds", .inMilliseconds),
   ("inSeconds", .inSeconds), ("durToText", .durToText)]
def setOps : List (String × SetOp) :=
  [("empty", .empty), ("size", .size), ("isEmpty", .isEmpty), ("has", .has), ("add", .add),
   ("remove", .remove), ("union", .union), ("intersection", .intersection),
   ("difference", .difference), ("toList", .toList), ("ofList", .ofList)]
def mapOps : List (String × MapOp) :=
  [("empty", .empty), ("size", .size), ("isEmpty", .isEmpty), ("get", .get), ("has", .has),
   ("set", .set), ("remove", .remove), ("keys", .keys), ("values", .values), ("entries", .entries),
   ("ofList", .ofList)]

def listOut : ListOp → Sexp
  | .lit n => tag "lit" [natural n]
  | .zip t => tag "zip" [tyOut t]
  | .size => tag "size" | .isEmpty => tag "isEmpty" | .get => tag "get"
  | .first => tag "first" | .last => tag "last" | .append => tag "append"
  | .prepend => tag "prepend" | .concat => tag "concat" | .reverse => tag "reverse"
  | .take => tag "take" | .drop => tag "drop" | .set => tag "set"
  | .removeAt => tag "removeAt" | .uncons => tag "uncons" | .enumerate => tag "enumerate"
  | .contains => tag "contains" | .unique => tag "unique"
def listIn : Sexp → Parse ListOp
  | .node [.atom "lit", n] => return .lit (← asNat n)
  | .node [.atom "zip", t] => return .zip (← tyIn t)
  | .node [.atom "size"] => pure .size | .node [.atom "isEmpty"] => pure .isEmpty
  | .node [.atom "get"] => pure .get | .node [.atom "first"] => pure .first
  | .node [.atom "last"] => pure .last | .node [.atom "append"] => pure .append
  | .node [.atom "prepend"] => pure .prepend | .node [.atom "concat"] => pure .concat
  | .node [.atom "reverse"] => pure .reverse | .node [.atom "take"] => pure .take
  | .node [.atom "drop"] => pure .drop | .node [.atom "set"] => pure .set
  | .node [.atom "removeAt"] => pure .removeAt | .node [.atom "uncons"] => pure .uncons
  | .node [.atom "enumerate"] => pure .enumerate | .node [.atom "contains"] => pure .contains
  | .node [.atom "unique"] => pure .unique
  | _ => throw "invalid ListOp"
def mapOut : MapOp → Sexp
  | .lit n => tag "lit" [natural n]
  | o => enumOut mapOps o
def mapIn : Sexp → Parse MapOp
  | .node [.atom "lit", n] => return .lit (← asNat n)
  | s => enumIn mapOps s
def ioOps : List (String × IoOp) :=
  [("argsList", .argsList), ("envGet", .envGet), ("print", .print), ("printError", .printError),
   ("readLine", .readLine), ("printed", .printed), ("printedErrors", .printedErrors),
   ("writeText", .writeText), ("appendText", .appendText), ("remove", .remove),
   ("makeFolder", .makeFolder),
   ("readOnly", .readOnly), ("now", .now), ("randomInt", .randomInt),
   ("consoleFake", .consoleFake), ("consoleFakeInput", .consoleFakeInput),
   ("filesInMemory", .filesInMemory), ("clockFixed", .clockFixed),
   ("randomSeeded", .randomSeeded), ("argsOf", .argsOf), ("envOf", .envOf)]
def ioOut : IoOp → Sexp
  | .readText ro => tag "readText" [boolean ro]
  | .readBytes ro => tag "readBytes" [boolean ro]
  | .list ro => tag "list" [boolean ro]
  | .entries ro => tag "entries" [boolean ro]
  | .«exists» ro => tag "exists" [boolean ro]
  | .within ro => tag "within" [boolean ro]
  | o => enumOut ioOps o
def ioIn : Sexp → Parse IoOp
  | .node [.atom "readText", ro] => return .readText (← asBool ro)
  | .node [.atom "readBytes", ro] => return .readBytes (← asBool ro)
  | .node [.atom "list", ro] => return .list (← asBool ro)
  | .node [.atom "entries", ro] => return .entries (← asBool ro)
  | .node [.atom "exists", ro] => return .«exists» (← asBool ro)
  | .node [.atom "within", ro] => return .within (← asBool ro)
  | s => enumIn ioOps s

def primOut : Prim → Sexp
  | .int o => tag "int" [enumOut intOps o]
  | .big o => tag "big" [enumOut bigOps o]
  | .byte o => tag "byte" [enumOut byteOps o]
  | .dec o => tag "dec" [enumOut decOps o]
  | .float o => tag "float" [enumOut floatOps o]
  | .not => tag "not"
  | .cmp b o => tag "cmp" [enumOut bases b, enumOut cmpOps o]
  | .char o => tag "char" [enumOut charOps o]
  | .text o => tag "text" [enumOut textOps o]
  | .bytes o => tag "bytes" [enumOut bytesOps o]
  | .unicode16 o => tag "unicode16" [enumOut unicodeOps o]
  | .range o => tag "range" [enumOut rangeOps o]
  | .time o => tag "time" [enumOut timeOps o]
  | .list t o => tag "list" [tyOut t, listOut o]
  | .map k v o => tag "map" [tyOut k, tyOut v, mapOut o]
  | .set t o => tag "set" [tyOut t, enumOut setOps o]
  | .eq t => tag "eq" [tyOut t]
  | .structural t => tag "structural" [tyOut t]
  | .io o => tag "io" [ioOut o]
def primIn : Sexp → Parse Prim
  | .node [.atom "int", o] => return .int (← enumIn intOps o)
  | .node [.atom "big", o] => return .big (← enumIn bigOps o)
  | .node [.atom "byte", o] => return .byte (← enumIn byteOps o)
  | .node [.atom "dec", o] => return .dec (← enumIn decOps o)
  | .node [.atom "float", o] => return .float (← enumIn floatOps o)
  | .node [.atom "not"] => pure .not
  | .node [.atom "cmp", b, o] => return .cmp (← enumIn bases b) (← enumIn cmpOps o)
  | .node [.atom "char", o] => return .char (← enumIn charOps o)
  | .node [.atom "text", o] => return .text (← enumIn textOps o)
  | .node [.atom "bytes", o] => return .bytes (← enumIn bytesOps o)
  | .node [.atom "unicode16", o] => return .unicode16 (← enumIn unicodeOps o)
  | .node [.atom "range", o] => return .range (← enumIn rangeOps o)
  | .node [.atom "time", o] => return .time (← enumIn timeOps o)
  | .node [.atom "list", t, o] => return .list (← tyIn t) (← listIn o)
  | .node [.atom "map", k, v, o] => return .map (← tyIn k) (← tyIn v) (← mapIn o)
  | .node [.atom "set", t, o] => return .set (← tyIn t) (← enumIn setOps o)
  | .node [.atom "eq", t] => return .eq (← tyIn t)
  | .node [.atom "structural", t] => return .structural (← tyIn t)
  | .node [.atom "io", o] => return .io (← ioIn o)
  | _ => throw "invalid primitive constructor or arity"

end MoCore.Text
