/-
  examples/json.mo in Core.

  Exercises: a recursive choice type (`Json` holds lists and maps of itself),
  recursion, patterns on characters (lowered to chains of `==`), `try` on
  `Result` and `Option`, a `case` used as a value (a join point), and a
  `while` loop over standard input. Every function of the sample is here.
-/
import MoCore.Testing

namespace MoCore.Examples.Json

open MoCore DSL Lib

def J : Ty := .data "Json"
def JE : Ty := .data "JsonError"

/-- ```
enum Json
  Null
  Boolean(Bool)
  Number(Decimal)
  String(Text)
  Array(List(Json))
  Object(Map(Text, Json))
end
``` -/
def Json : DataDecl :=
  choice "Json" [("Null", []), ("Boolean", [("0", tBool)]), ("Number", [("0", tDec)]),
    ("String", [("0", tText)]), ("Array", [("0", tList J)]), ("Object", [("0", tMap tText J)])]

/-- `enum JsonError  UnexpectedEnd  Unexpected(at: Int)  BadNumber(at: Int)  end` -/
def JsonError : DataDecl :=
  choice "JsonError" [("UnexpectedEnd", []), ("Unexpected", [("at", tInt)]), ("BadNumber", [("at", tInt)])]

def chars : Ty := tList tChar
def stepTy : Ty := tTup [J, tInt]
def textStepTy : Ty := tTup [tText, tInt]
def stepRes : Ty := tRes stepTy JE

def isC (x : Expr) (c : Char) : Expr := eq tChar x (chr c)
def at1 (e : Expr) : Expr := iadd e (int 1)
def getC (cs i : Expr) : Expr := lop tChar .get [cs, i]
def unexpectedEnd : Expr := mkC JsonError "UnexpectedEnd" []
def unexpected (pos : Expr) : Expr := mkC JsonError "Unexpected" [pos]
def skip (i : Expr) : Expr := call "skip_space" [v "chars", i]

/-- ```
fn to_text(problem: JsonError) : Text
  case problem
    UnexpectedEnd: "the JSON ends too soon"
    Unexpected(at: at): "unexpected character at position #{at}"
    BadNumber(at: at): "the number at position #{at} isn't one Mo reads"
  end
end
``` -/
def toText : FunDecl :=
  fn "JsonError.to_text" [("problem", JE)] tText <|
    .case_ (v "problem")
      [("UnexpectedEnd", [], str "the JSON ends too soon"),
       ("Unexpected", ["at"], cat [str "unexpected character at position ", intText (v "at")]),
       ("BadNumber", ["at"], cat [str "the number at position ", intText (v "at"), str " isn't one Mo reads"])]

/-- ```
fn parse(text: Text) : Result(Json, JsonError)
  chars = text.chars
  (value, after) = try value_at(chars, skip_space(chars, 0))
  rest = skip_space(chars, after)
  return Error(Unexpected(at: rest)) if rest < chars.size
  Ok(value)
end
``` -/
def parse : FunDecl :=
  fn "parse" [("text", tText)] (tRes J JE) <|
    lets [("chars", top .chars [v "text"])] <|
      tryRes J JE (call "value_at" [v "chars", skip (int 0)]) "%p" <|
        lets [("value", fst (v "%p")), ("after", snd (v "%p")), ("rest", skip (v "after"))] <|
          ite (ilt (v "rest") (lop tChar .size [v "chars"]))
            (err_ J JE (unexpected (v "rest")))
            (ok_ J JE (v "value"))

/-- ```
fn value_at(chars: List(Char), at: Int) : Result((Json, Int), JsonError)
  case chars.get(at)
    None: Error(UnexpectedEnd)
    Some('n'): word_at(chars, at, "null", Null)
    Some('t'): word_at(chars, at, "true", Boolean(true))
    Some('f'): word_at(chars, at, "false", Boolean(false))
    Some('"'):
      (text, after) = try string_at(chars, at + 1, [])
      Ok((String(text), after))
    Some('['): array_at(chars, skip_space(chars, at + 1), [])
    Some('{'): object_at(chars, skip_space(chars, at + 1), {})
    Some(c) if c == '-' or c.ascii_digit?: number_at(chars, at)
    Some(_): Error(Unexpected(at: at))
  end
end
``` -/
def valueAt : FunDecl :=
  fn "value_at" [("chars", chars), ("at", tInt)] stepRes <|
    caseOpt (getC (v "chars") (v "at")) "c"
      (ite (isC (v "c") 'n') (call "word_at" [v "chars", v "at", str "null", mkC Json "Null" []]) <|
       ite (isC (v "c") 't') (call "word_at" [v "chars", v "at", str "true", mkC Json "Boolean" [tt]]) <|
       ite (isC (v "c") 'f') (call "word_at" [v "chars", v "at", str "false", mkC Json "Boolean" [ff]]) <|
       ite (isC (v "c") '"')
         (tryRes stepTy JE (call "string_at" [v "chars", at1 (v "at"), nil tChar]) "%s" <|
           ok_ stepTy JE (tup [mkC Json "String" [fst (v "%s")], snd (v "%s")])) <|
       ite (isC (v "c") '[') (call "array_at" [v "chars", skip (at1 (v "at")), nil J]) <|
       ite (isC (v "c") '{') (call "object_at" [v "chars", skip (at1 (v "at")), emptyMap tText J]) <|
       ite (or_ (isC (v "c") '-') (prim (.char .isDigit) [v "c"])) (call "number_at" [v "chars", v "at"]) <|
         err_ stepTy JE (unexpected (v "at")))
      (err_ stepTy JE unexpectedEnd)

/-- ```
fn word_at(chars: List(Char), at: Int, word: Text, value: Json) : Result((Json, Int), JsonError)
  length = word.char_count
  found = chars.drop(at).take(length)
  return Error(UnexpectedEnd) if found.size < length
  return Error(Unexpected(at: at)) if Text.from_chars(found) != word
  Ok((value, at + length))
end
``` -/
def wordAt : FunDecl :=
  fn "word_at" [("chars", chars), ("at", tInt), ("word", tText), ("value", J)] stepRes <|
    lets [("length", top .charCount [v "word"]),
          ("found", lop tChar .take [lop tChar .drop [v "chars", v "at"], v "length"])] <|
      ite (ilt (lop tChar .size [v "found"]) (v "length")) (err_ stepTy JE unexpectedEnd) <|
      ite (ne tText (top .fromChars [v "found"]) (v "word")) (err_ stepTy JE (unexpected (v "at"))) <|
        ok_ stepTy JE (tup [v "value", iadd (v "at") (v "length")])

/-- ```
fn string_at(chars: List(Char), at: Int, read: List(Char)) : Result((Text, Int), JsonError)
  c = try chars.get(at).or_fail(UnexpectedEnd)
  case c
    '"': Ok((Text.from_chars(read), at + 1))
    '\\':
      escaped = try chars.get(at + 1).or_fail(UnexpectedEnd)
      if escaped == 'u'
        code = try hex_value(chars.drop(at + 2).take(4)).or_fail(Unexpected(at: at + 2))
        plain = try code.to_char.or_fail(Unexpected(at: at))
        string_at(chars, at + 6, read.append(plain))
      else
        plain = try unescape(escaped).or_fail(Unexpected(at: at + 1))
        string_at(chars, at + 2, read.append(plain))
      end
    _: string_at(chars, at + 1, read.append(c))
  end
end
``` -/
def stringAt : FunDecl :=
  fn "string_at" [("chars", chars), ("at", tInt), ("read", chars)] (tRes textStepTy JE) <|
    tryRes textStepTy JE (orFail tChar JE (getC (v "chars") (v "at")) unexpectedEnd) "c" <|
    ite (isC (v "c") '"') (ok_ textStepTy JE (tup [top .fromChars [v "read"], at1 (v "at")])) <|
    ite (isC (v "c") '\\')
      (tryRes textStepTy JE (orFail tChar JE (getC (v "chars") (at1 (v "at"))) unexpectedEnd) "escaped" <|
        ite (isC (v "escaped") 'u')
          (tryRes textStepTy JE
            (orFail tInt JE
              (call "hex_value" [lop tChar .take [lop tChar .drop [v "chars", iadd (v "at") (int 2)], int 4]])
              (unexpected (iadd (v "at") (int 2)))) "code" <|
           tryRes textStepTy JE (orFail tChar JE (iop .toChar [v "code"]) (unexpected (v "at"))) "plain" <|
             call "string_at" [v "chars", iadd (v "at") (int 6), lop tChar .append [v "read", v "plain"]])
          (tryRes textStepTy JE
            (orFail tChar JE (call "unescape" [v "escaped"]) (unexpected (at1 (v "at")))) "plain" <|
             call "string_at" [v "chars", iadd (v "at") (int 2), lop tChar .append [v "read", v "plain"]]))
      (call "string_at" [v "chars", at1 (v "at"), lop tChar .append [v "read", v "c"]])

/-- ```
fn hex_value(digits: List(Char)) : Option(Int)
  return None if digits.size != 4
  var value = 0
  for digit in digits
    value = value * 16 + try hex_digit_value(digit)
  end
  Some(value)
end
```
`value * 16` is evaluated before the `try`, left to right; a `None` from the
`try` returns from inside the loop. -/
def hexValue : FunDecl :=
  fn "hex_value" [("digits", chars)] (tOpt tInt) <|
    ite (ne tInt (lop tChar .size [v "digits"]) (int 4)) (none_ tInt) <|
    lets [("value", int 0)] <|
      forList "d" tChar "digit" (v "digits") [("value", tInt)] [v "value"]
        (lets [("%left", imul (v "value") (int 16))] <|
          tryOpt tInt (call "hex_digit_value" [v "digit"]) "%digit" <|
            nextRound "d" [iadd (v "%left") (v "%digit")])
        (some_ tInt (v "value"))

/-- ```
fn hex_digit_value(c: Char) : Option(Int)
  lower = c.ascii_lowercase
  if c.ascii_digit?
    Some(c.code - '0'.code)
  else if lower >= 'a' and lower <= 'f'
    Some(lower.code - 'a'.code + 10)
  else
    None
  end
end
``` -/
def hexDigitValue : FunDecl :=
  fn "hex_digit_value" [("c", tChar)] (tOpt tInt) <|
    lets [("lower", prim (.char .lower) [v "c"])] <|
      ite (prim (.char .isDigit) [v "c"])
        (some_ tInt (isub (prim (.char .code) [v "c"]) (prim (.char .code) [chr '0']))) <|
      ite (and_ (cmp .char .ge (v "lower") (chr 'a')) (cmp .char .le (v "lower") (chr 'f')))
        (some_ tInt (iadd (isub (prim (.char .code) [v "lower"]) (prim (.char .code) [chr 'a'])) (int 10)))
        (none_ tInt)

/-- ```
fn unescape(c: Char) : Option(Char)
  case c
    '"' | '\\' | '/': Some(c)
    'b': Some('\u{8}')
    'f': Some('\u{C}')
    'n': Some('\n')
    'r': Some('\r')
    't': Some('\t')
    _: None
  end
end
``` -/
def unescape : FunDecl :=
  fn "unescape" [("c", tChar)] (tOpt tChar) <|
    ite (or_ (isC (v "c") '"') (or_ (isC (v "c") '\\') (isC (v "c") '/'))) (some_ tChar (v "c")) <|
    ite (isC (v "c") 'b') (some_ tChar (chr '\x08')) <|
    ite (isC (v "c") 'f') (some_ tChar (chr '\x0c')) <|
    ite (isC (v "c") 'n') (some_ tChar (chr '\n')) <|
    ite (isC (v "c") 'r') (some_ tChar (chr '\r')) <|
    ite (isC (v "c") 't') (some_ tChar (chr '\t')) <|
      none_ tChar

/-- ```
fn number_at(chars: List(Char), at: Int) : Result((Json, Int), JsonError)
  after = number_end(chars, at)
  text = Text.from_chars(chars.drop(at).take(after - at))
  value = try text.to_decimal.or_fail(BadNumber(at: at))
  Ok((Number(value), after))
end
``` -/
def numberAt : FunDecl :=
  fn "number_at" [("chars", chars), ("at", tInt)] stepRes <|
    lets [("after", call "number_end" [v "chars", v "at"]),
          ("text", top .fromChars [lop tChar .take [lop tChar .drop [v "chars", v "at"],
            isub (v "after") (v "at")]])] <|
      tryRes stepTy JE (orFail tDec JE (top .toDecimal [v "text"]) (mkC JsonError "BadNumber" [v "at"]))
        "value" <|
        ok_ stepTy JE (tup [mkC Json "Number" [v "value"], v "after"])

/-- ```
fn number_end(chars: List(Char), at: Int) : Int
  case chars.get(at)
    Some(c) if c == '-' or c == '.' or c.ascii_digit?: number_end(chars, at + 1)
    Some(_) | None: at
  end
end
``` -/
def numberEnd : FunDecl :=
  fn "number_end" [("chars", chars), ("at", tInt)] tInt <|
    caseOpt (getC (v "chars") (v "at")) "c"
      (ite (or_ (isC (v "c") '-') (or_ (isC (v "c") '.') (prim (.char .isDigit) [v "c"])))
        (call "number_end" [v "chars", at1 (v "at")])
        (v "at"))
      (v "at")

/-- ```
fn array_at(chars: List(Char), at: Int, items: List(Json)) : Result((Json, Int), JsonError)
  return Ok((Array(items), at + 1)) if items.empty? and chars.get(at) == Some(']')
  (item, after) = try value_at(chars, at)
  next = skip_space(chars, after)
  case chars.get(next)
    Some(','): array_at(chars, skip_space(chars, next + 1), items.append(item))
    Some(']'): Ok((Array(items.append(item)), next + 1))
    Some(_): Error(Unexpected(at: next))
    None: Error(UnexpectedEnd)
  end
end
``` -/
def arrayAt : FunDecl :=
  fn "array_at" [("chars", chars), ("at", tInt), ("items", tList J)] stepRes <|
    ite (and_ (lop J .isEmpty [v "items"])
          (eq (tOpt tChar) (getC (v "chars") (v "at")) (some_ tChar (chr ']'))))
      (ok_ stepTy JE (tup [mkC Json "Array" [v "items"], at1 (v "at")])) <|
    tryRes stepTy JE (call "value_at" [v "chars", v "at"]) "%v" <|
    lets [("item", fst (v "%v")), ("after", snd (v "%v")), ("next", skip (v "after"))] <|
      caseOpt (getC (v "chars") (v "next")) "c"
        (ite (isC (v "c") ',')
          (call "array_at" [v "chars", skip (at1 (v "next")), lop J .append [v "items", v "item"]]) <|
         ite (isC (v "c") ']')
          (ok_ stepTy JE (tup [mkC Json "Array" [lop J .append [v "items", v "item"]], at1 (v "next")])) <|
          err_ stepTy JE (unexpected (v "next")))
        (err_ stepTy JE unexpectedEnd)

/-- ```
fn object_at(chars: List(Char), at: Int, fields: Map(Text, Json)) : Result((Json, Int), JsonError)
  return Ok((Object(fields), at + 1)) if fields.empty? and chars.get(at) == Some('}')
  key_at = try expect(chars, at, '"')
  (key, after_key) = try string_at(chars, key_at, [])
  value_start = try expect(chars, skip_space(chars, after_key), ':')
  (value, after_value) = try value_at(chars, skip_space(chars, value_start))
  next = skip_space(chars, after_value)
  more = fields.set(key, value)
  case chars.get(next)
    Some(','): object_at(chars, skip_space(chars, next + 1), more)
    Some('}'): Ok((Object(more), next + 1))
    Some(_): Error(Unexpected(at: next))
    None: Error(UnexpectedEnd)
  end
end
``` -/
def objectAt : FunDecl :=
  fn "object_at" [("chars", chars), ("at", tInt), ("fields", tMap tText J)] stepRes <|
    ite (and_ (mop tText J .isEmpty [v "fields"])
          (eq (tOpt tChar) (getC (v "chars") (v "at")) (some_ tChar (chr '}'))))
      (ok_ stepTy JE (tup [mkC Json "Object" [v "fields"], at1 (v "at")])) <|
    tryRes stepTy JE (call "expect" [v "chars", v "at", chr '"']) "key_at" <|
    tryRes stepTy JE (call "string_at" [v "chars", v "key_at", nil tChar]) "%k" <|
    lets [("key", fst (v "%k")), ("after_key", snd (v "%k"))] <|
    tryRes stepTy JE (call "expect" [v "chars", skip (v "after_key"), chr ':']) "value_start" <|
    tryRes stepTy JE (call "value_at" [v "chars", skip (v "value_start")]) "%v" <|
    lets [("value", fst (v "%v")), ("after_value", snd (v "%v")), ("next", skip (v "after_value")),
          ("more", mop tText J .set [v "fields", v "key", v "value"])] <|
      caseOpt (getC (v "chars") (v "next")) "c"
        (ite (isC (v "c") ',') (call "object_at" [v "chars", skip (at1 (v "next")), v "more"]) <|
         ite (isC (v "c") '}') (ok_ stepTy JE (tup [mkC Json "Object" [v "more"], at1 (v "next")])) <|
          err_ stepTy JE (unexpected (v "next")))
        (err_ stepTy JE unexpectedEnd)

/-- ```
fn expect(chars: List(Char), at: Int, wanted: Char) : Result(Int, JsonError)
  case chars.get(at)
    Some(c) if c == wanted: Ok(at + 1)
    Some(_): Error(Unexpected(at: at))
    None: Error(UnexpectedEnd)
  end
end
``` -/
def expect : FunDecl :=
  fn "expect" [("chars", chars), ("at", tInt), ("wanted", tChar)] (tRes tInt JE) <|
    caseOpt (getC (v "chars") (v "at")) "c"
      (ite (eq tChar (v "c") (v "wanted")) (ok_ tInt JE (at1 (v "at"))) (err_ tInt JE (unexpected (v "at"))))
      (err_ tInt JE unexpectedEnd)

/-- ```
fn skip_space(chars: List(Char), at: Int) : Int
  case chars.get(at)
    Some(c) if c.ascii_white_space?: skip_space(chars, at + 1)
    Some(_) | None: at
  end
end
``` -/
def skipSpace : FunDecl :=
  fn "skip_space" [("chars", chars), ("at", tInt)] tInt <|
    caseOpt (getC (v "chars") (v "at")) "c"
      (ite (prim (.char .isWhiteSpace) [v "c"]) (call "skip_space" [v "chars", at1 (v "at")]) (v "at"))
      (v "at")

/-- ```
fn pretty(value: Json, indent: Text) : Text
  inner = "#{indent}  "
  case value
    Null: "null"
    Boolean(b): "#{b}"
    Number(n): "#{n}"
    String(s): quoted(s)
    Array(items):
      return "[]" if items.empty?
      lines = items.map(fn(item) "#{inner}#{pretty(item, inner)}" end)
      "[\n#{lines.join(",\n")}\n#{indent}]"
    Object(fields):
      return "{}" if fields.empty?
      lines = fields.entries.map(fn(entry) "#{inner}#{quoted(entry.0)}: #{pretty(entry.1, inner)}" end)
      "{\n#{lines.join(",\n")}\n#{indent}}"
  end
end
``` -/
def pretty : FunDecl :=
  fn "pretty" [("value", J), ("indent", tText)] tText <|
    lets [("inner", cat [v "indent", str "  "])] <|
      .case_ (v "value")
        [("Null", [], str "null"),
         ("Boolean", ["b"], ite (v "b") (str "true") (str "false")),
         ("Number", ["n"], decText (v "n")),
         ("String", ["s"], call "quoted" [v "s"]),
         ("Array", ["items"],
           ite (lop J .isEmpty [v "items"]) (str "[]") <|
           lets [("lines", mapL J tText (v "items")
                   (lam [("item", J)] (cat [v "inner", call "pretty" [v "item", v "inner"]])))] <|
             cat [str "[\n", top .join [v "lines", str ",\n"], str "\n", v "indent", str "]"]),
         ("Object", ["fields"],
           ite (mop tText J .isEmpty [v "fields"]) (str "{}") <|
           lets [("lines", mapL (tTup [tText, J]) tText (mop tText J .entries [v "fields"])
                   (lam [("entry", tTup [tText, J])]
                     (cat [v "inner", call "quoted" [fst (v "entry")], str ": ",
                           call "pretty" [snd (v "entry"), v "inner"]])))] <|
             cat [str "{\n", top .join [v "lines", str ",\n"], str "\n", v "indent", str "}"])]

/-- ```
fn quoted(text: Text) : Text
  "\"#{text.chars.map(escape_char).join("")}\""
end
```
A named function used as a value becomes an anonymous function that calls it. -/
def quoted : FunDecl :=
  fn "quoted" [("text", tText)] tText <|
    cat [str "\"",
         top .join [mapL tChar tText (top .chars [v "text"]) (lam [("c", tChar)] (call "escape_char" [v "c"])),
                    str ""],
         str "\""]

/-- ```
fn escape_char(c: Char) : Text
  case c
    '"': "\\\""
    '\\': "\\\\"
    '\n': "\\n"
    '\r': "\\r"
    '\t': "\\t"
    '\u{8}': "\\b"
    '\u{C}': "\\f"
    _ if c.code < 32: "\\u00#{hex_digit(c.code / 16)}#{hex_digit(c.code % 16)}"
    _: c.to_text
  end
end
``` -/
def escapeChar : FunDecl :=
  fn "escape_char" [("c", tChar)] tText <|
    ite (isC (v "c") '"') (str "\\\"") <|
    ite (isC (v "c") '\\') (str "\\\\") <|
    ite (isC (v "c") '\n') (str "\\n") <|
    ite (isC (v "c") '\r') (str "\\r") <|
    ite (isC (v "c") '\t') (str "\\t") <|
    ite (isC (v "c") '\x08') (str "\\b") <|
    ite (isC (v "c") '\x0c') (str "\\f") <|
    ite (ilt (prim (.char .code) [v "c"]) (int 32))
      (cat [str "\\u00", call "hex_digit" [idiv (prim (.char .code) [v "c"]) (int 16)],
            call "hex_digit" [irem (prim (.char .code) [v "c"]) (int 16)]])
      (prim (.char .toText) [v "c"])

/-- ```
fn hex_digit(n: Int) : Text
  case "0123456789abcdef".chars.get(n)
    Some(digit): digit.to_text
    None: stop "a hexadecimal digit is from 0 to 15"
  end
end
``` -/
def hexDigit : FunDecl :=
  fn "hex_digit" [("n", tInt)] tText <|
    caseOpt (getC (top .chars [str "0123456789abcdef"]) (v "n")) "digit"
      (prim (.char .toText) [v "digit"])
      (stop "a hexadecimal digit is from 0 to 15")

/-- ```
fn at_path(value: Json, path: List(Text)) : Option(Json)
  case path
    []: Some(value)
    [step, ..rest]:
      inner = case value
        Object(fields): fields.get(step)
        Array(items): items.get(try step.to_int)
        Null | Boolean(_) | Number(_) | String(_): None
      end
      at_path(try inner, rest)
  end
end
```
`inner = case ...` makes the rest of the arm a join point, `%inner`; the
`try` in the `Array` arm returns `None` from the function. -/
def atPath : FunDecl :=
  fn "at_path" [("value", J), ("path", tList tText)] (tOpt J) <|
    caseOpt (lop tText .uncons [v "path"]) "%p"
      (lets [("step", fst (v "%p")), ("rest", snd (v "%p"))] <|
        .join "%inner" [("inner", tOpt J)]
          (tryOpt J (v "inner") "%found" (call "at_path" [v "%found", v "rest"]))
          (.case_ (v "value")
            [("Null", [], .jump "%inner" [none_ J]),
             ("Boolean", ["_"], .jump "%inner" [none_ J]),
             ("Number", ["_"], .jump "%inner" [none_ J]),
             ("String", ["_"], .jump "%inner" [none_ J]),
             ("Array", ["items"],
               tryOpt J (top .toInt [v "step"]) "%i" (.jump "%inner" [lop J .get [v "items", v "%i"]])),
             ("Object", ["fields"], .jump "%inner" [mop tText J .get [v "fields", v "step"]])]))
      (some_ J (v "value"))

/-- ```
fn read_input(console: Console) : Text
  var lines = []
  while console.read_line is Some(line)
    lines = lines.append(line)
  end
  lines.join("\n")
end
``` -/
def readInput : FunDecl :=
  fn "read_input" [("console", tCap .console)] tText <|
    lets [("lines", nil tText)] <|
      whileLoop "w" [("lines", tList tText)] [v "lines"]
        (caseOpt (io .readLine [v "console"]) "line"
          (nextWhile "w" [lop tText .append [v "lines", v "line"]])
          (breakLoop "w" [v "lines"]))
        (top .join [v "lines", str "\n"])

/-- ```
fn main(args: Args, console: Console) : Int
  path = case args.list
    []: []
    [steps]: steps.split(".")
    _:
      console.error("usage: json [PATH] < FILE.json")
      return 2
  end
  case parse(read_input(console))
    Error(problem):
      console.error("json: #{problem}")
      1
    Ok(value): show(at_path(value, path), console)
  end
end
``` -/
def mainFn : FunDecl :=
  fn "main" [("args", tCap .args), ("console", tCap .console)] tInt <|
    .join "%path" [("path", tList tText)]
      (caseRes (call "parse" [call "read_input" [v "console"]])
        "value" (call "show" [call "at_path" [v "value", v "path"], v "console"])
        "problem"
          (seq (printError (v "console") (cat [str "json: ", call "JsonError.to_text" [v "problem"]]))
            (int 1)))
      (lets [("%list", argsList (v "args"))] <|
        .join "%usage" [] (seq (printError (v "console") (str "usage: json [PATH] < FILE.json")) (int 2)) <|
          matchList tText (v "%list") [] none (.jump "%path" [nil tText]) <|
          matchList tText (v "%list") ["steps"] none (.jump "%path" [top .split [v "steps", str "."]]) <|
            .jump "%usage" [])

/-- ```
fn show(found: Option(Json), console: Console) : Int
  case found
    Some(value):
      console.print(pretty(value, ""))
      0
    None:
      console.error("json: nothing is at that path")
      1
  end
end
``` -/
def showFn : FunDecl :=
  fn "show" [("found", tOpt J), ("console", tCap .console)] tInt <|
    caseOpt (v "found") "value"
      (seq (print (v "console") (call "pretty" [v "value", str ""])) (int 0))
      (seq (printError (v "console") (str "json: nothing is at that path")) (int 1))

def program : Program :=
  { data := [Json, JsonError]
    funs := [toText, parse, valueAt, wordAt, stringAt, hexValue, hexDigitValue, unescape, numberAt,
             numberEnd, arrayAt, objectAt, expect, skipSpace, pretty, quoted, escapeChar, hexDigit,
             atPath, readInput, mainFn, showFn,
             Lib.map J tText, Lib.map (tTup [tText, J]) tText, Lib.map tChar tText] }

/-- The checker accepts the program; `decide +kernel` has the kernel run it. -/
theorem checked : wellTyped? program = true := by decide +kernel

/-- So the program is well typed, and `type_safety` applies to it. -/
theorem wellTyped : WellTyped program := checkProgram_sound checked

/-! ### Examples -/

def jNull : Expr := mkC Json "Null" []
def jBool (b : Bool) : Expr := mkC Json "Boolean" [bool b]
def jNum (s : String) : Expr := mkC Json "Number" [dec s]
def jStr (s : String) : Expr := mkC Json "String" [str s]
def jArr (es : List Expr) : Expr := mkC Json "Array" [listOf J es]
def jObj (kvs : List (String × Expr)) : Expr := mkC Json "Object" [mapOf tText J (kvs.map fun (k, e) => (str k, e))]
def parsed (s : String) : Expr := call "parse" [str s]
def okJ (e : Expr) : Expr := ok_ J JE e
def errJ (e : Expr) : Expr := err_ J JE e

-- parse(" [1, true, null] ") == Ok(Array([Number(1), Boolean(true), Null]))
#guard passes program (eq (tRes J JE) (parsed " [1, true, null] ") (okJ (jArr [jNum "1", jBool true, jNull])))
-- parse("{\"a\": {\"b\": \"c\"}}") == Ok(Object({"a": Object({"b": String("c")})}))
#guard passes program
  (eq (tRes J JE) (parsed "{\"a\": {\"b\": \"c\"}}") (okJ (jObj [("a", jObj [("b", jStr "c")])])))
-- parse("[1,") == Error(UnexpectedEnd)
#guard passes program (eq (tRes J JE) (parsed "[1,") (errJ unexpectedEnd))
-- parse("[1] 2") == Error(Unexpected(at: 4))
#guard passes program (eq (tRes J JE) (parsed "[1] 2") (errJ (unexpected (int 4))))
-- parse("\"caf\\u00e9\"") == Ok(String("café"))
#guard passes program (eq (tRes J JE) (parsed "\"caf\\u00e9\"") (okJ (jStr "café")))
-- hex_value(['0', '0', 'e', '9']) == Some(233)
#guard passes program
  (eq (tOpt tInt) (call "hex_value" [listOf tChar [chr '0', chr '0', chr 'e', chr '9']]) (some_ tInt (int 233)))
-- hex_value(['1', '2']) == None
#guard passes program (eq (tOpt tInt) (call "hex_value" [listOf tChar [chr '1', chr '2']]) (none_ tInt))
-- hex_value(['0', '0', 'g', '0']) == None
#guard passes program
  (eq (tOpt tInt) (call "hex_value" [listOf tChar [chr '0', chr '0', chr 'g', chr '0']]) (none_ tInt))
-- pretty(Array([]), "") == "[]"
#guard passes program (teq (call "pretty" [jArr [], str ""]) (str "[]"))
-- pretty(Object({"a": Array([Number(1.5)])}), "") == "{\n  \"a\": [\n    1.5\n  ]\n}"
#guard passes program
  (teq (call "pretty" [jObj [("a", jArr [jNum "1.5"])], str ""]) (str "{\n  \"a\": [\n    1.5\n  ]\n}"))
-- quoted("say \"hi\"\n") == "\"say \\\"hi\\\"\\n\""
#guard passes program (teq (call "quoted" [str "say \"hi\"\n"]) (str "\"say \\\"hi\\\"\\n\""))
-- quoted("tab\tbell\u{7}") == "\"tab\\tbell\\u0007\""
#guard passes program (teq (call "quoted" [str "tab\tbell\x07"]) (str "\"tab\\tbell\\u0007\""))
-- at_path(Object({"items": Array([Null, Object({"name": String("pen")})])}), ["items", "1", "name"])
--   == Some(String("pen"))
#guard passes program
  (eq (tOpt J)
    (call "at_path" [jObj [("items", jArr [jNull, jObj [("name", jStr "pen")]])], strs ["items", "1", "name"]])
    (some_ J (jStr "pen")))
-- at_path(Array([Null]), ["5"]) == None
#guard passes program (eq (tOpt J) (call "at_path" [jArr [jNull], strs ["5"]]) (none_ J))
-- at_path(Null, []) == Some(Null)
#guard passes program (eq (tOpt J) (call "at_path" [jNull, nil tText]) (some_ J jNull))

/-! ### Test "a path picks out one value"

```
console = Console.fake_input(["{\"items\": [{\"name\": \"pen\"}]}"])
assert main(Args.of(["items.0.name"]), console) == 0
assert console.printed == ["\"pen\""]
``` -/
#guard passes program
  (and_ (ieq (call "main" [v "args", v "console"]) (int 0))
    (eq (tList tText) (io .printed [v "console"]) (strs ["\"pen\""])))
  { args := ["items.0.name"], stdin := ["{\"items\": [{\"name\": \"pen\"}]}"] }
  [("args", .args), ("console", .console)]

/-! ### More: a whole document laid out, and a parse failure reported by `main` -/

#guard passes program
  (and_ (ieq (call "main" [v "args", v "console"]) (int 0))
    (eq (tList tText) (io .printed [v "console"])
      (strs ["{\n  \"name\": \"pen\",\n  \"tags\": [\n    true,\n    null,\n    -0.25\n  ],\n  \"empty\": {}\n}"])))
  { stdin := ["{\"name\": \"pen\",", " \"tags\": [true, null, -0.250], \"empty\": {}}"] }
  [("args", .args), ("console", .console)]

#guard passes program
  (and_ (ieq (call "main" [v "args", v "console"]) (int 1))
    (eq (tList tText) (io .printedErrors [v "console"])
      (strs ["json: unexpected character at position 6"])))
  { stdin := ["[1, 2 3]"] }
  [("args", .args), ("console", .console)]

/-! ### Never wrong -/

/-- Running `main` never goes wrong, whatever the arguments and the input. -/
theorem main_never_wrong (n : Nat) (w : World) (msg : String) :
    eval n program (handles [("args", .args), ("console", .console)]) w
      (call "main" [v "args", v "console"]) ≠ .wrong msg :=
  never_wrong_with_handles (τ := tInt) checked (by decide +kernel) n w msg

/-- Parsing any text never goes wrong: it gives a `Json`, a `JsonError`, or a
bug report (such as running out of fuel on deep input). -/
theorem parse_never_wrong {ρ : Env} (hρ : EnvTy program ρ [("text", tText)]) (n : Nat) (w : World)
    (msg : String) : eval n program ρ w (call "parse" [v "text"]) ≠ .wrong msg :=
  never_wrong (τ := tRes J JE) checked (by decide +kernel) hρ n w msg

end MoCore.Examples.Json
