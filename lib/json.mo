## JSON as RFC 8259 defines it: reading it into a Json value, and writing one back.
module Json
expose Json(..),
JsonNumber,
JsonError(..),
JsonProblem(..),
parse,
int,
decimal,
float,
to_text,
pretty,
to_int,
to_decimal,
to_float

enum Json
  Null
  Boolean(Bool)
  Number(JsonNumber)
  String(Text)
  Array(List(Json))
  Object(Map(Text, Json))
end

## A number as it was written. Two are equal when they're written the same.
struct JsonNumber
  text: Text
end

## Where reading stopped, counting lines and columns from 1, and why.
struct JsonError
  line: Int
  column: Int
  problem: JsonProblem
end

enum JsonProblem
  UnexpectedEnd
  Unexpected(found: Char)
  BadNumber
  BadEscape
  DuplicateKey(key: Text)
  TooDeep
end

## Where reading stopped, as a position in the document's characters.
struct Halt
  at: Int
  problem: JsonProblem
end

## The JSON value the text holds, with nothing but white space around it.
fn parse(text: Text) : Result(Json, JsonError)
  examples
    parse(" [1, true, null] ") == Ok(Array([int(1), Boolean(true), Null]))
    parse("{\"a\": {\"b\": \"c\"}}") == Ok(Object({"a": Object({"b": String("c")})}))
    parse("\"caf\\u00e9 \\ud83d\\ude00\"") == Ok(String("café 😀"))
    parse("[1,") == Error(JsonError(line: 1, column: 4, problem: UnexpectedEnd))
    parse("[1]\n x") == Error(JsonError(line: 2, column: 2, problem: Unexpected(found: 'x')))
    parse("{\"a\": 1, \"a\": 2}") ==
      Error(JsonError(line: 1, column: 10, problem: DuplicateKey(key: "a")))
    parse("01") == Error(JsonError(line: 1, column: 2, problem: BadNumber))
    parse("\"\\x\"") == Error(JsonError(line: 1, column: 2, problem: BadEscape))
    parse("[".repeat(129)) == Error(JsonError(line: 1, column: 129, problem: TooDeep))
  end

  chars = text.chars
  document(chars).map_error(fn(halt) located(chars, halt) end)
end

fn document(chars: List(Char)) : Result(Json, Halt)
  (value, after) = try value_at(chars, skip_space(chars, 0), 0)
  rest = skip_space(chars, after)
  case chars.get(rest)
    Some(c): Error(Halt(at: rest, problem: Unexpected(found: c)))
    None: Ok(value)
  end
end

## The halt's position as a line and a column, each counted from 1.
fn located(chars: List(Char), halt: Halt) : JsonError
  var line = 1
  var column = 1
  for c in chars.take(halt.at)
    if c == '\n'
      line += 1
      column = 1
    else
      column += 1
    end
  end
  JsonError(line: line, column: column, problem: halt.problem)
end

fn ended(chars: List(Char)) : Halt
  Halt(at: chars.size, problem: UnexpectedEnd)
end

## The halt at a character that can't be there, or at the end.
fn unexpected(chars: List(Char), at: Int) : Halt
  case chars.get(at)
    Some(c): Halt(at: at, problem: Unexpected(found: c))
    None: ended(chars)
  end
end

## JSON's white space: space, tab, line feed and carriage return.
fn skip_space(chars: List(Char), from: Int) : Int
  var at = from
  while true
    case chars.get(at)
      Some(' ') | Some('\t') | Some('\n') | Some('\r'): at += 1
      Some(_) | None: return at
    end
  end
  at
end

## The value starting at a character that isn't white space, inside `depth` arrays and
## objects, and the position after it. Arrays and objects nest at most 128 deep (reference 14).
fn value_at(chars: List(Char), at: Int, depth: Int) : Result((Json, Int), Halt)
  case chars.get(at)
    None: Error(ended(chars))
    Some('n'): word_at(chars, at, "null", Null)
    Some('t'): word_at(chars, at, "true", Boolean(true))
    Some('f'): word_at(chars, at, "false", Boolean(false))
    Some('"'):
      (text, after) = try string_at(chars, at)
      Ok((String(text), after))
    Some('['):
      return Error(Halt(at: at, problem: TooDeep)) if depth >= 128
      array_at(chars, at, depth + 1)
    Some('{'):
      return Error(Halt(at: at, problem: TooDeep)) if depth >= 128
      object_at(chars, at, depth + 1)
    Some(c) if c == '-' or c.ascii_digit?: number_at(chars, at)
    Some(c): Error(Halt(at: at, problem: Unexpected(found: c)))
  end
end

fn word_at(chars: List(Char), at: Int, word: Text, value: Json) : Result((Json, Int), Halt)
  var next = at
  for wanted in word.chars
    return Error(unexpected(chars, next)) if chars.get(next) != Some(wanted)
    next += 1
  end
  Ok((value, next))
end

## A string's text, from its opening quote at `quote`, and the position after its closing quote.
fn string_at(chars: List(Char), quote: Int) : Result((Text, Int), Halt)
  var read = []
  var at = quote + 1
  while true
    c = try chars.get(at).or_fail(ended(chars))
    if c == '"'
      return Ok((Text.from_chars(read), at + 1))
    else if c == '\\'
      (plain, after) = try escape_at(chars, at)
      read = read.append(plain)
      at = after
    else if c.code < 32
      return Error(Halt(at: at, problem: Unexpected(found: c)))
    else
      read = read.append(c)
      at += 1
    end
  end
  Ok((Text.from_chars(read), at))
end

## The character an escape at `at` stands for, and the position after it. A `\u` escape of a
## surrogate needs its pair right after it.
fn escape_at(chars: List(Char), at: Int) : Result((Char, Int), Halt)
  bad = Halt(at: at, problem: BadEscape)
  escaped = try chars.get(at + 1).or_fail(ended(chars))
  if escaped != 'u'
    plain = try unescape(escaped).or_fail(bad)
    return Ok((plain, at + 2))
  end
  code = try hex_at(chars, at + 2, bad)
  if code >= 0xD800 and code < 0xDC00
    return Error(bad) if chars.get(at + 6) != Some('\\') or chars.get(at + 7) != Some('u')
    low = try hex_at(chars, at + 8, bad)
    return Error(bad) if low < 0xDC00 or low >= 0xE000
    joined = try (0x10000 + (code - 0xD800) * 0x400 + (low - 0xDC00)).to_char.or_fail(bad)
    return Ok((joined, at + 12))
  end
  plain = try code.to_char.or_fail(bad)
  Ok((plain, at + 6))
end

## The value of the four hexadecimal digits at `at`.
fn hex_at(chars: List(Char), at: Int, bad: Halt) : Result(Int, Halt)
  var value = 0
  for i in at..at + 4
    digit = try chars.get(i).or_fail(ended(chars))
    value = value * 16 + try hex_digit_value(digit).or_fail(bad)
  end
  Ok(value)
end

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

## A number: an optional minus, an integer part with no leading zero, then an optional fraction
## and exponent.
fn number_at(chars: List(Char), start: Int) : Result((Json, Int), Halt)
  var at = start
  if chars.get(at) == Some('-')
    at += 1
  end
  if chars.get(at) == Some('0')
    at += 1
  else
    at = try digits_at(chars, at)
  end
  if chars.get(at) == Some('.')
    at = try digits_at(chars, at + 1)
  end
  if chars.get(at) == Some('e') or chars.get(at) == Some('E')
    at += 1
    if chars.get(at) == Some('+') or chars.get(at) == Some('-')
      at += 1
    end
    at = try digits_at(chars, at)
  end
  return Error(Halt(at: at, problem: BadNumber)) if digit_at?(chars, at)
  Ok((Number(JsonNumber(text: slice(chars, start, at))), at))
end

## The characters from `start` up to `after` as a text. Unlike `drop`, it doesn't copy the rest of
## the document.
fn slice(chars: List(Char), start: Int, after: Int) : Text
  var part = []
  for i in start..after
    case chars.get(i)
      Some(c): part = part.append(c)
      None: stop "a slice ends inside the document"
    end
  end
  Text.from_chars(part)
end

## The position after one or more digits starting at `at`.
fn digits_at(chars: List(Char), from: Int) : Result(Int, Halt)
  return Error(Halt(at: from, problem: BadNumber)) if not digit_at?(chars, from)
  var at = from
  while digit_at?(chars, at)
    at += 1
  end
  Ok(at)
end

fn digit_at?(chars: List(Char), at: Int) : Bool
  case chars.get(at)
    Some(c): c.ascii_digit?
    None: false
  end
end

## The rest of an array, from its opening bracket.
fn array_at(chars: List(Char), bracket: Int, depth: Int) : Result((Json, Int), Halt)
  var items = []
  var at = skip_space(chars, bracket + 1)
  return Ok((Array(items), at + 1)) if chars.get(at) == Some(']')
  while true
    (item, after) = try value_at(chars, at, depth)
    items = items.append(item)
    next = skip_space(chars, after)
    case chars.get(next)
      Some(','): at = skip_space(chars, next + 1)
      Some(']'): return Ok((Array(items), next + 1))
      Some(_) | None: return Error(unexpected(chars, next))
    end
  end
  Ok((Array(items), at))
end

## The rest of an object, from its opening brace. A key given twice is an error.
fn object_at(chars: List(Char), brace: Int, depth: Int) : Result((Json, Int), Halt)
  var fields = {}
  var at = skip_space(chars, brace + 1)
  return Ok((Object(fields), at + 1)) if chars.get(at) == Some('}')
  while true
    return Error(unexpected(chars, at)) if chars.get(at) != Some('"')
    (key, after_key) = try string_at(chars, at)
    return Error(Halt(at: at, problem: DuplicateKey(key: key))) if fields.has?(key)
    colon = skip_space(chars, after_key)
    return Error(unexpected(chars, colon)) if chars.get(colon) != Some(':')
    (value, after) = try value_at(chars, skip_space(chars, colon + 1), depth)
    fields = fields.set(key, value)
    next = skip_space(chars, after)
    case chars.get(next)
      Some(','): at = skip_space(chars, next + 1)
      Some('}'): return Ok((Object(fields), next + 1))
      Some(_) | None: return Error(unexpected(chars, next))
    end
  end
  Ok((Object(fields), at))
end

## The number n.
fn int(n: Int) : Json
  examples
    int(-12).to_text == "-12"
  end

  Number(JsonNumber(text: n.to_text))
end

## The number d, written exactly.
fn decimal(d: Decimal) : Json
  examples
    decimal(1.50).to_text == "1.5"
  end

  Number(JsonNumber(text: d.to_text))
end

## The number f, or None for not-a-number and the infinities, which JSON can't write.
fn float(f: Float) : Option(Json)
  examples
    float(0.5).map(fn(json) json.to_text end) == Some("0.5")
    float(1.0 / 0.0) == None
  end

  text = f.to_text
  return None if text == "nan" or text == "inf" or text == "-inf"
  Some(Number(JsonNumber(text: text)))
end

## The number as an Int, or None when it isn't a whole number that fits one.
fn to_int(number: JsonNumber) : Option(Int)
  examples
    JsonNumber(text: "-12").to_int == Some(-12)
    JsonNumber(text: "1.5").to_int == None
    JsonNumber(text: "1.50e1").to_int == Some(15)
    JsonNumber(text: "99999999999999999999").to_int == None
  end

  (negative, whole, fraction) = try plain(number)
  return None if not fraction.empty?
  "#{if negative: "-" else: ""}#{Text.from_chars(whole)}".to_int
end

## The number as a Decimal, or None when it doesn't fit one: when it needs more than 18 decimal
## places, or is beyond the Decimal range.
fn to_decimal(number: JsonNumber) : Option(Decimal)
  examples
    JsonNumber(text: "0.1").to_decimal == Some(0.1)
    JsonNumber(text: "-2.5E-3").to_decimal == Some(-0.0025)
    JsonNumber(text: "1e-19").to_decimal == None
  end

  (negative, whole, fraction) = try plain(number)
  sign = if negative: "-" else: ""
  return "#{sign}#{Text.from_chars(whole)}".to_decimal if fraction.empty?
  "#{sign}#{Text.from_chars(whole)}.#{Text.from_chars(fraction)}".to_decimal
end

## The number written without an exponent: whether it is below zero, its whole digits without
## leading zeros (at least one), and its fraction digits without trailing zeros. None when it
## is too far from 1 to fit an Int or a Decimal.
fn plain(number: JsonNumber) : Option((Bool, List(Char), List(Char)))
  examples
    plain(JsonNumber(text: "-001.2300")) == Some((true, ['1'], ['2', '3']))
    plain(JsonNumber(text: "12.5e-3")) == Some((false, ['0'], ['0', '1', '2', '5']))
    plain(JsonNumber(text: "0.0e999999999999999999999")) == Some((false, ['0'], []))
    plain(JsonNumber(text: "1e999999999999999999999")) == None
  end

  chars = number.text.chars
  negative = chars.first == Some('-')
  var digits = []
  var point = 0
  var seen_point = false
  var exponent_text = ""
  for (i, c) in chars.enumerate
    if c == 'e' or c == 'E'
      exponent_text = Text.from_chars(chars.drop(i + 1))
      break
    else if c == '.'
      seen_point = true
    else if c.ascii_digit?
      digits = digits.append(c)
      if not seen_point
        point += 1
      end
    end
  end
  return Some((negative, ['0'], [])) if digits.all?(fn(d) d == '0' end)
  while digits.first == Some('0')
    digits = digits.drop(1)
    point -= 1
  end
  exponent = if exponent_text.empty?: 0 else: try (exponent_text.drop_prefix("+") or exponent_text).to_int
  return None if exponent > 60 - point or exponent < -60 - point
  point += exponent
  var whole = []
  var fraction = []
  if point <= 0
    fraction = zeros(0 - point).concat(digits)
  else if point >= digits.size
    whole = digits.concat(zeros(point - digits.size))
  else
    whole = digits.take(point)
    fraction = digits.drop(point)
  end
  while fraction.last == Some('0')
    fraction = fraction.take(fraction.size - 1)
  end
  Some((negative, if whole.empty?: ['0'] else: whole, fraction))
end

fn zeros(count: Int) : List(Char)
  var made = []
  for _i in 0..count
    made = made.append('0')
  end
  made
end

## The nearest Float, or None when the number is too large for one.
fn to_float(number: JsonNumber) : Option(Float)
  examples
    JsonNumber(text: "2.5e3").to_float == Some(2500.0)
    JsonNumber(text: "1e999").to_float == None
  end

  number.text.to_float
end

## The number as it was written.
fn to_text(number: JsonNumber) : Text
  number.text
end

## The value as compact JSON, keeping each object's order and each number's text.
fn to_text(value: Json) : Text
  examples
    Object({"a": Array([int(1), Null]), "b": String("x\"y")}).to_text ==
      "{\"a\":[1,null],\"b\":\"x\\\"y\"}"
  end

  case value
    Null: "null"
    Boolean(b): b.to_text
    Number(n): n.text
    String(s): quoted(s)
    Array(items): "[#{items.map(fn(item) item.to_text end).join(",")}]"
    Object(fields):
      parts = fields.entries.map(fn(entry) "#{quoted(entry.0)}:#{entry.1.to_text}" end)
      "{#{parts.join(",")}}"
  end
end

## The value as JSON indented by two spaces at each level.
fn pretty(value: Json) : Text
  examples
    Array([]).pretty == "[]"
    Object({"a": Array([int(1), Object({})])}).pretty == "{\n  \"a\": [\n    1,\n    {}\n  ]\n}"
  end

  laid_out(value, "")
end

fn laid_out(value: Json, indent: Text) : Text
  inner = "#{indent}  "
  case value
    Array(items) if not items.empty?:
      lines = items.map(fn(item) "#{inner}#{laid_out(item, inner)}" end)
      "[\n#{lines.join(",\n")}\n#{indent}]"
    Object(fields) if not fields.empty?:
      lines = fields.entries.map(
        fn(entry) "#{inner}#{quoted(entry.0)}: #{laid_out(entry.1, inner)}" end
      )
      "{\n#{lines.join(",\n")}\n#{indent}}"
    Null | Boolean(_) | Number(_) | String(_) | Array(_) | Object(_): value.to_text
  end
end

## The text as a JSON string.
fn quoted(text: Text) : Text
  examples
    quoted("say \"hi\"\n") == "\"say \\\"hi\\\"\\n\""
    quoted("tab\tbell\u{7}/é") == "\"tab\\tbell\\u0007/é\""
  end

  "\"#{text.chars.map(escape_char).join("")}\""
end

## A character as JSON writes it inside a string.
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

fn hex_digit(n: Int) : Text
  case "0123456789abcdef".chars.get(n)
    Some(digit): digit.to_text
    None: stop "a hexadecimal digit is from 0 to 15"
  end
end

fn problem_at(text: Text) : (Int, Int, Text)
  case parse(text)
    Ok(_): (0, 0, "parsed")
    Error(e): (e.line, e.column, "#{e.problem}")
  end
end

test "reading stops at the character that can't be there"
  assert problem_at("") == (1, 1, "UnexpectedEnd")
  assert problem_at("  \n ") == (2, 2, "UnexpectedEnd")
  assert problem_at("tru") == (1, 4, "UnexpectedEnd")
  assert problem_at("trux") == (1, 4, "Unexpected(found: 'x')")
  assert problem_at("[1,]") == (1, 4, "Unexpected(found: ']')")
  assert problem_at("{\"a\": 1,}") == (1, 9, "Unexpected(found: '}')")
  assert problem_at("{1: 2}") == (1, 2, "Unexpected(found: '1')")
  assert problem_at("{\"a\" 1}") == (1, 6, "Unexpected(found: '1')")
  assert problem_at("[1 2]") == (1, 4, "Unexpected(found: '2')")
  assert problem_at("\u{B}1") == (1, 1, "Unexpected(found: '\\u{b}')")
  assert problem_at("\"a\tb\"") == (1, 3, "Unexpected(found: '\\t')")
  assert problem_at("\"abc") == (1, 5, "UnexpectedEnd")
  assert problem_at("[\n  1,\n  x\n]") == (3, 3, "Unexpected(found: 'x')")
end

test "numbers follow JSON's own syntax"
  assert problem_at("-") == (1, 2, "BadNumber")
  assert problem_at("1.") == (1, 3, "BadNumber")
  assert problem_at(".5") == (1, 1, "Unexpected(found: '.')")
  assert problem_at("-01") == (1, 3, "BadNumber")
  assert problem_at("1e") == (1, 3, "BadNumber")
  assert problem_at("1e+") == (1, 4, "BadNumber")
  assert problem_at("+1") == (1, 1, "Unexpected(found: '+')")
  assert problem_at("1.5x") == (1, 4, "Unexpected(found: 'x')")
  assert parse("[0, -0, 10, 1.25, -2.5E-3, 4e+2]").map(fn(json) json.to_text end) ==
    Ok("[0,-0,10,1.25,-2.5E-3,4e+2]")
end

test "escapes, surrogate pairs and lone surrogates"
  assert parse("\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"") == Ok(String("\"\\/\u{8}\u{C}\n\r\t"))
  assert parse("\"\\u0041\\u00E9\\uD834\\uDD1E\"") == Ok(String("Aé𝄞"))
  assert problem_at("\"\\ud834\"") == (1, 2, "BadEscape")
  assert problem_at("\"\\ud834\\u0041\"") == (1, 2, "BadEscape")
  assert problem_at("\"\\udd1e\"") == (1, 2, "BadEscape")
  assert problem_at("\"\\u12g4\"") == (1, 2, "BadEscape")
  assert problem_at("\"\\u12") == (1, 6, "UnexpectedEnd")
  assert problem_at("\"\\") == (1, 3, "UnexpectedEnd")
end

test "arrays and objects nest 128 deep"
  deep = "#{"[".repeat(128)}#{"]".repeat(128)}"
  assert parse(deep).map(fn(json) json.to_text end) == Ok(deep)
  assert problem_at("#{"{\"a\":".repeat(128)}{}") == (1, 641, "TooDeep")
  assert problem_at("[{\"a\": [1], \"b\": {\"a\": 2}, \"a\": 3}]") == (
    1,
    28,
    "DuplicateKey(key: \"a\")"
  )
end

fn number(text: Text) : JsonNumber
  JsonNumber(text: text)
end

test "a number converts when its value fits, however it is written"
  assert number("9223372036854775807").to_int == Some(9223372036854775807)
  assert number("9223372036854775808").to_int == None
  assert number("9.223372036854775807e18").to_int == Some(9223372036854775807)
  assert number("-9223372036854775808").to_int == Some(-9223372036854775807 - 1)
  assert number("1200e-2").to_int == Some(12)
  assert number("1201e-2").to_int == None
  assert number("-0.0e5").to_int == Some(0)
  assert number("1e60").to_int == None
  assert number("1e99999999999999999999").to_int == None
  assert number("0.000000000000000001").to_decimal == Some(0.000000000000000001)
  assert number("1e20").to_decimal == Some(100000000000000000000.0)
  assert number("1e21").to_decimal == None
  assert number("123.456e-1").to_decimal == Some(12.3456)
  assert number("1e-1000").to_float == Some(0.0)
  assert number("-1E+2").to_float == Some(-100.0)
end

test "objects keep the document's order, and numbers their text"
  json = parse("{\"z\": 1.50, \"a\": [], \"m\": {\"k\": \"v\"}}")
  assert json.map(fn(value) value.to_text end) == Ok("{\"z\":1.50,\"a\":[],\"m\":{\"k\":\"v\"}}")
  assert json.map(fn(value) value.pretty end) ==
    Ok("{\n  \"z\": 1.50,\n  \"a\": [],\n  \"m\": {\n    \"k\": \"v\"\n  }\n}")
  assert parse("1.50") != parse("1.5")
  assert decimal(1.50) == decimal(1.5)
  assert parse("1.5") == Ok(decimal(1.5))
end
