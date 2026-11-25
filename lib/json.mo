## JSON as RFC 8259 defines it: reading it into a Json value, and writing one back.
module Json
expose Json(..),
JsonNumber,
JsonError(..),
JsonProblem(..),
JsonDecodeError(..),
parse,
int,
decimal,
float,
to_text,
pretty,
encode,
encode_pretty,
decode,
decode_strict,
from_json,
from_json_strict,
schema,
to_int,
to_decimal,
to_float,
get,
at,
path,
text_at,
int_at,
bool_at,
list_at

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

## Why decoding JSON into a typed Mo value stopped.
enum JsonDecodeError
  NotUtf8
  Syntax(JsonError)
  Mismatch(path: Text, message: Text)
end

## The decode error as a readable message.
fn to_text(error: JsonDecodeError) : Text
  case error
    NotUtf8: "JSON input isn't valid UTF-8"
    Syntax(e): "JSON syntax error at #{e.line}:#{e.column}: #{e.problem}"
    Mismatch(path: p, message: m): if p == "": m else: "#{p}: #{m}"
  end
end

## The value encoded as compact JSON text.
fn encode(value: T) : Text where T: Json
  value.to_json.to_text
end

## The value encoded as JSON indented by two spaces at each level.
fn encode_pretty(value: T) : Text where T: Json
  value.to_json.pretty
end

## Decodes a parsed Json value into T, ignoring unknown object fields.
fn from_json(value: Json) : Result(T, JsonDecodeError) where T: Json
  if value == Null
    stop "compiler-lowered Json.from_json"
  else
    stop "compiler-lowered Json.from_json"
  end
end

## Decodes a parsed Json value into T, rejecting unknown object fields.
fn from_json_strict(value: Json) : Result(T, JsonDecodeError) where T: Json
  if value == Null
    stop "compiler-lowered Json.from_json_strict"
  else
    stop "compiler-lowered Json.from_json_strict"
  end
end

## Decodes UTF-8 JSON text or bytes into T, ignoring unknown object fields.
fn decode(input: Text) : Result(T, JsonDecodeError) where T: Json
  if input == ""
    stop "compiler-lowered Json.decode"
  else
    stop "compiler-lowered Json.decode"
  end
end

## Decodes UTF-8 JSON text or bytes into T, rejecting unknown object fields.
fn decode_strict(input: Text) : Result(T, JsonDecodeError) where T: Json
  if input == ""
    stop "compiler-lowered Json.decode_strict"
  else
    stop "compiler-lowered Json.decode_strict"
  end
end

## The JSON Schema object describing values of type T.
fn schema() : Json where T: Json
  Object({})
end

fn parse_text_input(text: Text) : Result(Json, JsonDecodeError)
  parse(text).map_error(fn(e) Syntax(e) end)
end

fn parse_bytes_input(bytes: Bytes) : Result(Json, JsonDecodeError)
  text = try bytes.decode_utf8.or_fail(NotUtf8)
  parse_text_input(text)
end

fn encode_byte(b: Byte) : Json
  int(b.to_int)
end

fn encode_float(f: Float) : Json
  if f.nan? or f.infinite?
    stop "non-finite Float has no JSON number"
  end
  Number(JsonNumber(text: f.to_text))
end

fn encode_big_int(n: BigInt) : Json
  String(n.to_text)
end

fn encode_decimal_str(d: Decimal) : Json
  String(d.to_text)
end

fn encode_char(c: Char) : Json
  String(c.to_text)
end

fn encode_time(t: Time) : Json
  String(t.to_text)
end

fn json_kind_name(v: Json) : Text
  case v
    Null: "null"
    Boolean(_): "boolean"
    Number(_): "number"
    String(_): "string"
    Array(_): "array"
    Object(_): "object"
  end
end

fn join_path(prefix: Text, step: Text) : Text
  if prefix == ""
    step
  else
    "#{prefix}.#{step}"
  end
end

fn index_path(prefix: Text, idx: Int) : Text
  join_path(prefix, idx.to_text)
end

fn mismatch_at(at_path: Text, expected: Text, v: Json) : JsonDecodeError
  Mismatch(path: at_path, message: "expected #{expected}, found #{json_kind_name(v)}")
end

fn decode_unit(v: Json, at_path: Text) : Result(Unit, JsonDecodeError)
  case v
    Null: Ok(Unit)
    Boolean(_) | Number(_) | String(_) | Array(_) | Object(_): Error(
      mismatch_at(at_path, "null", v)
    )
  end
end

fn decode_bool(v: Json, at_path: Text) : Result(Bool, JsonDecodeError)
  case v
    Boolean(b): Ok(b)
    Null | Number(_) | String(_) | Array(_) | Object(_): Error(mismatch_at(at_path, "boolean", v))
  end
end

fn decode_int(v: Json, at_path: Text) : Result(Int, JsonDecodeError)
  case v
    Number(num): num.to_int.or_fail(
      Mismatch(path: at_path, message: "expected 64-bit integer, found #{num.text}")
    )
    Null | Boolean(_) | String(_) | Array(_) | Object(_): Error(mismatch_at(at_path, "integer", v))
  end
end

fn decode_byte(v: Json, at_path: Text) : Result(Byte, JsonDecodeError)
  case v
    Number(num):
      n = try num.to_int.or_fail(
        Mismatch(path: at_path, message: "expected byte 0..255, found #{num.text}")
      )
      n.to_byte.or_fail(Mismatch(path: at_path, message: "expected byte 0..255, found #{num.text}"))
    Null | Boolean(_) | String(_) | Array(_) | Object(_): Error(mismatch_at(at_path, "byte", v))
  end
end

fn decode_float(v: Json, at_path: Text) : Result(Float, JsonDecodeError)
  case v
    Number(num):
      f = try num.to_float.or_fail(
        Mismatch(path: at_path, message: "number #{num.text} overflows Float")
      )
      return Error(Mismatch(path: at_path, message: "number #{num.text} overflows Float")) if f.nan? or
        f.infinite?
      Ok(f)
    Null | Boolean(_) | String(_) | Array(_) | Object(_): Error(mismatch_at(at_path, "number", v))
  end
end

fn decode_decimal(v: Json, at_path: Text) : Result(Decimal, JsonDecodeError)
  case v
    String(s): s.to_decimal.or_fail(
      Mismatch(path: at_path, message: "expected Decimal string, found #{quoted(s)}")
    )
    Null | Boolean(_) | Number(_) | Array(_) | Object(_): Error(
      mismatch_at(at_path, "Decimal string", v)
    )
  end
end

fn parse_big_int(s: Text) : Option(BigInt)
  chars = s.chars
  negative = chars.first == Some('-')
  digits = if negative: chars.drop(1) else: chars
  return None if digits.empty?
  var acc = 0.to_big
  ten = 10.to_big
  for c in digits
    return None if not c.ascii_digit?
    acc = acc * ten + (c.code - '0'.code).to_big
  end
  Some(if negative: -acc else: acc)
end

fn decode_big_int(v: Json, at_path: Text) : Result(BigInt, JsonDecodeError)
  case v
    String(s): parse_big_int(s).or_fail(
      Mismatch(path: at_path, message: "expected BigInt string, found #{quoted(s)}")
    )
    Null | Boolean(_) | Number(_) | Array(_) | Object(_): Error(
      mismatch_at(at_path, "BigInt string", v)
    )
  end
end

fn decode_text(v: Json, at_path: Text) : Result(Text, JsonDecodeError)
  case v
    String(s): Ok(s)
    Null | Boolean(_) | Number(_) | Array(_) | Object(_): Error(mismatch_at(at_path, "string", v))
  end
end

fn decode_char(v: Json, at_path: Text) : Result(Char, JsonDecodeError)
  case v
    String(s):
      chars = s.chars
      if chars.size == 1
        case chars.first
          Some(c): Ok(c)
          None: stop "single-character list has first"
        end
      else
        Error(Mismatch(path: at_path, message: "expected 1-character string, found #{quoted(s)}"))
      end
    Null | Boolean(_) | Number(_) | Array(_) | Object(_): Error(
      mismatch_at(at_path, "1-character string", v)
    )
  end
end

fn decode_time(v: Json, at_path: Text) : Result(Time, JsonDecodeError)
  case v
    String(s): Time.parse(s).or_fail(
      Mismatch(path: at_path, message: "expected RFC 3339 timestamp, found #{quoted(s)}")
    )
    Null | Boolean(_) | Number(_) | Array(_) | Object(_): Error(
      mismatch_at(at_path, "RFC 3339 timestamp string", v)
    )
  end
end

fn decode_json_number(v: Json, at_path: Text) : Result(JsonNumber, JsonDecodeError)
  case v
    Number(num): Ok(num)
    Null | Boolean(_) | String(_) | Array(_) | Object(_): Error(mismatch_at(at_path, "number", v))
  end
end

fn expect_object(v: Json, at_path: Text) : Result(Map(Text, Json), JsonDecodeError)
  case v
    Object(fields): Ok(fields)
    Null | Boolean(_) | Number(_) | String(_) | Array(_): Error(mismatch_at(at_path, "object", v))
  end
end

fn expect_array(v: Json, at_path: Text) : Result(List(Json), JsonDecodeError)
  case v
    Array(items): Ok(items)
    Null | Boolean(_) | Number(_) | String(_) | Object(_): Error(mismatch_at(at_path, "array", v))
  end
end

fn expect_tuple_array(v: Json, expected_size: Int, at_path: Text) : Result(
  List(Json),
  JsonDecodeError
)
  items = try expect_array(v, at_path)
  if items.size != expected_size
    return Error(
      Mismatch(
        path: at_path,
        message: "expected array of #{expected_size} items, found #{items.size}"
      )
    )
  end
  Ok(items)
end

fn missing_field_error(at_path: Text, field: Text) : JsonDecodeError
  Mismatch(path: join_path(at_path, field), message: "missing field '#{field}'")
end

fn unknown_case_error(v: Json, at_path: Text, type_name: Text) : JsonDecodeError
  Mismatch(path: at_path, message: "invalid #{type_name} case #{v.to_text}")
end

fn wrap_custom_error(res: Result(T, Text), at_path: Text) : Result(T, JsonDecodeError)
  res.map_error(fn(msg) Mismatch(path: at_path, message: msg) end)
end

fn check_strict_keys(fields: Map(Text, Json), present: Int, known: List(Text), at_path: Text) : Result(
  Unit,
  JsonDecodeError
)
  return Ok(Unit) if fields.size == present
  for (key, _) in fields.entries
    if not known.contains?(key)
      return Error(Mismatch(path: join_path(at_path, key), message: "unknown field '#{key}'"))
    end
  end
  Ok(Unit)
end

fn encode_list(items: List(T), to_item: fn(T) : Json) : Json
  Array(items.map(to_item))
end

fn encode_map(entries: Map(Text, V), to_val: fn(V) : Json) : Json
  var out = {}
  for (k, v) in entries.entries
    out = out.set(k, to_val(v))
  end
  Object(out)
end

fn encode_option(opt: Option(T), to_item: fn(T) : Json) : Json
  case opt
    None: Null
    Some(v): to_item(v)
  end
end

fn decode_list(
  v: Json,
  at_path: Text,
  strict: Bool,
  from_item: fn(Json, Text, Bool) : Result(T, JsonDecodeError)
) : Result(List(T), JsonDecodeError)
  items = try expect_array(v, at_path)
  var out = []
  for (i, item) in items.enumerate
    decoded = try from_item(item, index_path(at_path, i), strict)
    out = out.append(decoded)
  end
  Ok(out)
end

fn decode_map(
  v: Json,
  at_path: Text,
  strict: Bool,
  from_val: fn(Json, Text, Bool) : Result(V, JsonDecodeError)
) : Result(Map(Text, V), JsonDecodeError)
  fields = try expect_object(v, at_path)
  var out = {}
  for (k, val) in fields.entries
    decoded = try from_val(val, join_path(at_path, k), strict)
    out = out.set(k, decoded)
  end
  Ok(out)
end

fn decode_option(
  v: Json,
  at_path: Text,
  strict: Bool,
  from_item: fn(Json, Text, Bool) : Result(T, JsonDecodeError)
) : Result(Option(T), JsonDecodeError)
  case v
    Null: Ok(None)
    Boolean(_) | Number(_) | String(_) | Array(_) | Object(_):
      decoded = try from_item(v, at_path, strict)
      Ok(Some(decoded))
  end
end

fn schema_ref(name: Text) : Json
  Object({"$ref": String("#/$defs/#{name}")})
end

fn schema_type(kind: Text) : Json
  Object({"type": String(kind)})
end

fn schema_format(kind: Text, fmt: Text) : Json
  Object({"type": String(kind), "format": String(fmt)})
end

fn schema_byte() : Json
  Object({"type": String("integer"), "minimum": int(0), "maximum": int(255)})
end

fn schema_char() : Json
  Object({"type": String("string"), "minLength": int(1), "maxLength": int(1)})
end

fn schema_array(item_schema: Json) : Json
  Object({"type": String("array"), "items": item_schema})
end

fn schema_tuple(item_schemas: List(Json)) : Json
  Object({
    "type": String("array"),
    "prefixItems": Array(item_schemas),
    "minItems": int(item_schemas.size),
    "maxItems": int(item_schemas.size)
  })
end

fn schema_map(val_schema: Json) : Json
  Object({"type": String("object"), "additionalProperties": val_schema})
end

fn schema_nullable(inner: Json) : Json
  Object({"anyOf": Array([inner, Object({"type": String("null")})])})
end

fn schema_object(props: Map(Text, Json), required: List(Text)) : Json
  Object({
    "type": String("object"),
    "properties": Object(props),
    "required": Array(required.map(fn(r) String(r) end))
  })
end

fn schema_enum_strings(cases: List(Text)) : Json
  Object({"type": String("string"), "enum": Array(cases.map(fn(c) String(c) end))})
end

fn schema_any_of(variants: List(Json)) : Json
  Object({"anyOf": Array(variants)})
end

fn schema_tagged_case(tag: Text, payload: Json) : Json
  Object({
    "type": String("object"),
    "properties": Object({tag: payload}),
    "required": Array([String(tag)]),
    "additionalProperties": Boolean(false)
  })
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

  case Json.parse_raw(text)
    Ok((tags, texts)):
      (value, _, _) = materialize_value(tags, texts, 0, 0)
      Ok(value)
    Error((line, column, kind, detail)): Error(
      JsonError(line: line, column: column, problem: decode_problem(kind, detail))
    )
  end
end

fn decode_problem(kind: Int, detail: Text) : JsonProblem
  case kind
    0: UnexpectedEnd
    1: Unexpected(found: detail.chars.first or '\u{0}')
    2: BadNumber
    3: BadEscape
    4: DuplicateKey(key: detail)
    5: TooDeep
    _: stop "unknown JSON parse error kind"
  end
end

fn materialize_value(tags: List(Int), texts: List(Text), tag_pos: Int, text_pos: Int) : (
  Json,
  Int,
  Int
)
  tag = tags.get(tag_pos) or stop "valid JSON token tag"
  if tag == 0
    (Null, tag_pos + 1, text_pos)
  else if tag == 1
    (Boolean(false), tag_pos + 1, text_pos)
  else if tag == 2
    (Boolean(true), tag_pos + 1, text_pos)
  else if tag == 3
    num = texts.get(text_pos) or stop "valid JSON number text"
    (Number(JsonNumber(text: num)), tag_pos + 1, text_pos + 1)
  else if tag == 4
    s = texts.get(text_pos) or stop "valid JSON string text"
    (String(s), tag_pos + 1, text_pos + 1)
  else if tag >= 10 and tag % 2 == 0
    count = (tag - 10) / 2
    var items = []
    var tp = tag_pos + 1
    var sp = text_pos
    for _i in 0..count
      (item, next_tp, next_sp) = materialize_value(tags, texts, tp, sp)
      items = items.append(item)
      tp = next_tp
      sp = next_sp
    end
    (Array(items), tp, sp)
  else if tag >= 11 and tag % 2 == 1
    count = (tag - 11) / 2
    var fields = {}
    var tp = tag_pos + 1
    var sp = text_pos
    for _i in 0..count
      key = texts.get(sp) or stop "valid JSON object key"
      (val, next_tp, next_sp) = materialize_value(tags, texts, tp, sp + 1)
      fields = fields.set(key, val)
      tp = next_tp
      sp = next_sp
    end
    (Object(fields), tp, sp)
  else
    stop "unknown JSON token tag"
  end
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

  case number.text.to_int
    Some(n): return Some(n)
    None: Unit
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

  case number.text.to_decimal
    Some(d): return Some(d)
    None: Unit
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
    plain(JsonNumber(text: "1e59")).map(fn(p) p.1.size end) == Some(60)
    plain(JsonNumber(text: "1e60")) == None
    plain(JsonNumber(text: "1e-61")).map(fn(p) p.2.size end) == Some(61)
    plain(JsonNumber(text: "1e-62")) == None
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
  # Each loop finds a position and the list is cut once, since every drop and take copies it.
  var lead = 0
  while digits.get(lead) == Some('0')
    lead += 1
  end
  digits = digits.drop(lead)
  point -= lead
  unsigned = exponent_text.drop_prefix("+") or exponent_text
  exponent = if exponent_text.empty?: 0 else: try unsigned.to_int
  # No Int or Decimal has a digit 60 places either side of the point.
  return None if exponent > 60 - point or exponent < -60 - point
  point += exponent
  padded = zeros(0 - point).concat(digits).concat(zeros(point))
  split = point.max(0)
  whole = padded.take(split)
  var fraction = padded.drop(split)
  var kept = fraction.size
  while fraction.get(kept - 1) == Some('0')
    kept -= 1
  end
  fraction = fraction.take(kept)
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
    Boolean(b): if b: "true" else: "false"
    Number(n): n.text
    String(s): Json.quote_raw(s)
    Array(_) | Object(_): write_compact(value, []).join("")
  end
end

fn write_compact(value: Json, parts: List(Text)) : List(Text)
  case value
    Null: parts.append("null")
    Boolean(b): parts.append(if b: "true" else: "false")
    Number(n): parts.append(n.text)
    String(s): parts.append(Json.quote_raw(s))
    Array(items):
      var out = parts.append("[")
      var first = true
      for item in items
        if not first
          out = out.append(",")
        end
        first = false
        out = write_compact(item, out)
      end
      out.append("]")
    Object(fields):
      var out = parts.append("{")
      var first = true
      for (k, v) in fields.entries
        if not first
          out = out.append(",")
        end
        first = false
        out = out.append(Json.quote_raw(k)).append(":")
        out = write_compact(v, out)
      end
      out.append("}")
  end
end

## The value as JSON indented by two spaces at each level.
fn pretty(value: Json) : Text
  examples
    Array([]).pretty == "[]"
    Object({"a": Array([int(1), Object({})])}).pretty == "{\n  \"a\": [\n    1,\n    {}\n  ]\n}"
  end

  write_pretty(value, "", []).join("")
end

fn write_pretty(value: Json, indent: Text, parts: List(Text)) : List(Text)
  case value
    Array(items) if not items.empty?:
      inner = "#{indent}  "
      var out = parts.append("[\n")
      var first = true
      for item in items
        if not first
          out = out.append(",\n")
        end
        first = false
        out = write_pretty(item, inner, out.append(inner))
      end
      out.append("\n").append(indent).append("]")
    Object(fields) if not fields.empty?:
      inner = "#{indent}  "
      var out = parts.append("{\n")
      var first = true
      for (k, v) in fields.entries
        if not first
          out = out.append(",\n")
        end
        first = false
        out = write_pretty(v, inner, out.append(inner).append(Json.quote_raw(k)).append(": "))
      end
      out.append("\n").append(indent).append("}")
    Null | Boolean(_) | Number(_) | String(_) | Array(_) | Object(_): write_compact(value, parts)
  end
end

## The text as a JSON string.
fn quoted(text: Text) : Text
  examples
    quoted("say \"hi\"\n") == "\"say \\\"hi\\\"\\n\""
    quoted("tab\tbell\u{7}/é") == "\"tab\\tbell\\u0007/é\""
    quoted("\u{1f}\u{0} ") == "\"\\u001f\\u0000 \""
    quoted("\u{10}") == "\"\\u0010\""
  end

  Json.quote_raw(text)
end

## The field of an object with this key, or None when the value isn't an object that has it.
fn get(value: Json, key: Text) : Option(Json)
  examples
    Object({"a": Null}).get("a") == Some(Null)
    Object({"a": Null}).get("b") == None
    Array([Null]).get("0") == None
  end

  case value
    Object(fields): fields.get(key)
    Null | Boolean(_) | Number(_) | String(_) | Array(_): None
  end
end

## The item of an array at this index, counted from 0, or None when the value isn't an array
## that long.
fn at(value: Json, index: Int) : Option(Json)
  examples
    Array([Null, int(7)]).at(0) == Some(Null)
    Array([Null, int(7)]).at(1) == Some(int(7))
    Array([Null]).at(1) == None
    Array([Null]).at(-1) == None
    Object({"0": Null}).at(0) == None
  end

  case value
    Array(items): items.get(index)
    Null | Boolean(_) | Number(_) | String(_) | Object(_): None
  end
end

## The value at a path of steps separated by `.`, such as `items.0.user.login`. A step is a key
## in an object and an index, counted from 0, in an array. The empty path is the value itself.
fn path(value: Json, steps: Text) : Option(Json)
  examples
    Object({"items": Array([Object({"user": Object({"login": String("mo")})})])}).path(
      "items.0.user.login"
    ) ==
      Some(String("mo"))
    Array([Null]).path("") == Some(Array([Null]))
    Array([Null]).path("5") == None
    Object({"0": int(1)}).path("0") == Some(int(1))
    String("pen").path("0") == None
  end

  if steps == ""
    Some(value)
  else
    walked(value, steps.split("."))
  end
end

fn walked(value: Json, steps: List(Text)) : Option(Json)
  case steps
    []: Some(value)
    [step, ..rest]:
      inner = case value
        Object(fields): fields.get(step)
        Array(items): items.get(try index(step))
        Null | Boolean(_) | Number(_) | String(_): None
      end
      walked(try inner, rest)
  end
end

## A step as an index: ASCII digits and nothing else.
fn index(step: Text) : Option(Int)
  examples
    index("10") == Some(10)
    index("-0") == None
    index("x") == None
  end

  if step.starts_with?("-")
    None
  else
    step.to_int
  end
end

## The string at a path, or None when the path is absent or holds another kind of value.
fn text_at(value: Json, steps: Text) : Option(Text)
  examples
    Object({"name": String("pen")}).text_at("name") == Some("pen")
    Object({"name": int(1)}).text_at("name") == None
    Object({}).text_at("name") == None
  end

  if path(value, steps) is Some(String(text))
    Some(text)
  else
    None
  end
end

## The whole number at a path, or None when the path is absent, holds another kind of value, or
## holds a number that isn't a whole number that fits an Int.
fn int_at(value: Json, steps: Text) : Option(Int)
  examples
    Array([int(7)]).int_at("0") == Some(7)
    Array([decimal(1.5)]).int_at("0") == None
    Array([String("7")]).int_at("0") == None
  end

  if path(value, steps) is Some(Number(number))
    number.to_int
  else
    None
  end
end

## The true or false at a path, or None when the path is absent or holds another kind of value.
fn bool_at(value: Json, steps: Text) : Option(Bool)
  examples
    Object({"open": Boolean(false)}).bool_at("open") == Some(false)
    Object({"open": Null}).bool_at("open") == None
  end

  if path(value, steps) is Some(Boolean(flag))
    Some(flag)
  else
    None
  end
end

## The items of the array at a path, or None when the path is absent or holds another kind of
## value.
fn list_at(value: Json, steps: Text) : Option(List(Json))
  examples
    Array([Null]).list_at("") == Some([Null])
    Object({"items": Array([])}).list_at("items") == Some([])
    Object({"items": Null}).list_at("items") == None
  end

  if path(value, steps) is Some(Array(items))
    Some(items)
  else
    None
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
  assert parse(" \t\r\n[\t1\r,\n2 ]\r\n").map(fn(json) json.to_text end) == Ok("[1,2]")
  assert problem_at("\"a\u{0}b\"") == (1, 3, "Unexpected(found: '\\u{0}')")
  assert parse("false") == Ok(Boolean(false))
  assert problem_at("[]") == (0, 0, "parsed")
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
  assert parse("1e5").map(fn(json) json.to_text end) == Ok("1e5")
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
  assert parse("\"\\uD800\\uDC00\\u00aF\"") == Ok(String("\u{10000}\u{af}"))
  assert problem_at("\"\\uD800\\uE000\"") == (1, 2, "BadEscape")
  assert problem_at("\"\\uDC00\\u12") == (1, 2, "BadEscape")
  assert problem_at("\"\\ud834\\nDD1E\"") == (1, 2, "BadEscape")
end

test "a string with an escape leaves the strings after it as they are"
  assert parse("[\"a\\\"b\", \"c\", \"d\\\\\", \"e\"]").map(fn(json) json.to_text end) ==
    Ok("[\"a\\\"b\",\"c\",\"d\\\\\",\"e\"]")
  assert parse("{\"k\\\"\": \"v\", \"w\": \"x\\u0022y\", \"z\": \"\"}").map(
    fn(json) json.to_text end
  ) ==
    Ok("{\"k\\\"\":\"v\",\"w\":\"x\\\"y\",\"z\":\"\"}")
  assert problem_at("[\"a\", \"b\nc\"]") == (1, 9, "Unexpected(found: '\\n')")
  assert problem_at("[\"a\\\"\", \"b") == (1, 11, "UnexpectedEnd")
end

test "arrays and objects nest 128 deep"
  deep = "#{"[".repeat(128)}#{"]".repeat(128)}"
  assert parse(deep).map(fn(json) json.to_text end) == Ok(deep)
  assert problem_at("#{"{\"a\":".repeat(128)}{}") == (1, 641, "TooDeep")
  assert parse("[{},{ }]").map(fn(json) json.to_text end) == Ok("[{},{}]")
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
  assert number("1e999999999").to_int == None
  assert number("1e-999999999").to_decimal == None
  assert number("1#{"0".repeat(100000)}e-100000").to_int == Some(1)
  assert number("0.#{"0".repeat(100000)}1e100001").to_decimal == Some(1.0)
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

struct UserPatch
  name: Text
  bio: Option(Text)
  age: Option(Int)
end

struct TeamPayload
  team: Text
  members: List(UserPatch)
end

enum JobState
  Pending
  InProgress
  HTTPSError
  RetryAfter(Int)
  Halted(code: Int, detail: Option(Text))
end

enum IntTree
  Leaf(Int)
  Branch(left: IntTree, right: IntTree)
end

fn round_trip_strict(value: T) : Result(T, JsonDecodeError) where T: Json
  decode_strict[T](encode(value))
end

fn round_trip_value_strict(value: T) : Result(T, JsonDecodeError) where T: Json
  from_json_strict[T](value.to_json)
end

test "derived Json ability round-trips scalars, collections and tuples"
  assert round_trip_strict(Unit) == Ok(Unit)
  assert round_trip_strict(true) == Ok(true)
  assert round_trip_strict(-42) == Ok(-42)
  b255 = 255.to_byte or stop "valid byte"
  assert round_trip_strict(b255) == Ok(b255)
  assert decode[Byte]("256") is Error(Mismatch(path: "", message: _))
  assert decode[Byte]("-1") is Error(Mismatch(path: "", message: _))
  assert round_trip_strict(3.5) == Ok(3.5)
  assert round_trip_strict(12.340) == Ok(12.34)
  assert encode(12.34) == "\"12.34\""
  assert decode[Decimal]("12.34") is Error(Mismatch(path: "", message: _))
  big = 10.to_big.pow(30) + 7.to_big
  assert round_trip_strict(big) == Ok(big)
  assert round_trip_strict(-big) == Ok(-big)
  assert round_trip_strict('é') == Ok('é')
  assert decode[Char]("\"ab\"") is Error(Mismatch(path: "", message: _))
  stamp = Time.parse("2026-03-15T10:30:00Z") or stop "valid timestamp"
  assert round_trip_strict(stamp) == Ok(stamp)
  assert round_trip_strict([1, 2, 3]) == Ok([1, 2, 3])
  assert round_trip_strict(["a", "b"].to_set) == Ok(["a", "b"].to_set)
  assert round_trip_strict({"k": 9, "m": 4}) == Ok({"k": 9, "m": 4})
  assert round_trip_strict(("ada", 7, false)) == Ok(("ada", 7, false))
  assert decode[(Int, Int)]("[1]") ==
    Error(Mismatch(path: "", message: "expected array of 2 items, found 1"))
end

test "structs support Option omission, lenient vs strict decoding, Bytes input and dotted paths"
  assert decode[UserPatch]("{\"name\":\"Ada\"}") == Ok(UserPatch(name: "Ada", bio: None, age: None))
  assert decode_strict[UserPatch]("{\"name\":\"Ada\"}") ==
    Ok(UserPatch(name: "Ada", bio: None, age: None))
  assert decode_strict[UserPatch]("{\"name\":\"Ada\",\"bio\":null,\"age\":37}") ==
    Ok(UserPatch(name: "Ada", bio: None, age: Some(37)))
  lenient = "{\"name\":\"Ada\",\"extra\":true}"
  assert decode[UserPatch](lenient) == Ok(UserPatch(name: "Ada", bio: None, age: None))
  assert decode_strict[UserPatch](lenient) ==
    Error(Mismatch(path: "extra", message: "unknown field 'extra'"))
  nested_bad = "{\"team\":\"core\",\"members\":[{\"name\":\"Ada\"},{\"name\":99}]}"
  assert decode[TeamPayload](nested_bad) ==
    Error(Mismatch(path: "members.1.name", message: "expected string, found number"))
  assert decode[UserPatch]("{\"name\":\"Grace\"}".bytes) ==
    Ok(UserPatch(name: "Grace", bio: None, age: None))
  bad_utf8 = "x".bytes.slice(0, 0)
  assert decode[UserPatch](bad_utf8) is Error(Syntax(_))
end

test "enums encode snake_case strings and single-key tagged payloads"
  assert encode(Pending) == "\"pending\""
  assert encode(InProgress) == "\"in_progress\""
  assert encode(HTTPSError) == "\"https_error\""
  assert encode(RetryAfter(15)) == "{\"retry_after\":15}"
  assert encode(Halted(code: 502, detail: Some("bad gateway"))) ==
    "{\"halted\":{\"code\":502,\"detail\":\"bad gateway\"}}"
  assert round_trip_strict(InProgress) == Ok(InProgress)
  assert round_trip_strict(HTTPSError) == Ok(HTTPSError)
  assert round_trip_strict(RetryAfter(15)) == Ok(RetryAfter(15))
  assert decode_strict[JobState]("{\"halted\":{\"code\":502}}") ==
    Ok(Halted(code: 502, detail: None))
  assert decode_strict[JobState]("{\"halted\":{\"code\":502,\"extra\":1}}") ==
    Error(Mismatch(path: "halted.extra", message: "unknown field 'extra'"))
end

test "recursive types, first-class adapters and schema work"
  tree = Branch(left: Leaf(1), right: Branch(left: Leaf(2), right: Leaf(3)))
  assert round_trip_strict(tree) == Ok(tree)
  assert ["10", "20"].map(decode[Int]) == [Ok(10), Ok(20)]
  assert [1, 2].map(Int.to_json) == [int(1), int(2)]
  patch_schema = schema[UserPatch]()
  assert patch_schema.text_at("type") == Some("object")
  assert patch_schema.list_at("required") == Some([String("name")])
  tree_schema = schema[IntTree]()
  assert tree_schema.list_at("anyOf").map(fn(xs) xs.size end) == Some(2)
end

test "from_json and from_json_strict decode Json values directly, by dot call and as adapters"
  doc = Object({"name": String("Ada"), "extra": Boolean(true)})
  expected_patch = UserPatch(name: "Ada", bio: None, age: None)
  assert from_json[UserPatch](doc) == Ok(expected_patch)
  assert doc.from_json[UserPatch] == Ok(expected_patch)
  assert from_json(doc) == Ok(expected_patch)
  assert [int(3), int(4)].map(fn(v) v.from_json end) == [Ok(3), Ok(4)]
  assert from_json_strict[UserPatch](doc) ==
    Error(Mismatch(path: "extra", message: "unknown field 'extra'"))
  assert doc.from_json_strict[UserPatch] ==
    Error(Mismatch(path: "extra", message: "unknown field 'extra'"))
  assert from_json[Json](doc) == Ok(doc)
  assert from_json_strict[Json](doc) == Ok(doc)
  tree = Branch(left: Leaf(1), right: Leaf(2))
  assert round_trip_value_strict(tree) == Ok(tree)
  assert [int(10), int(20)].map(from_json[Int]) == [Ok(10), Ok(20)]
  assert [doc].map(from_json_strict[UserPatch]) == [
    Error(Mismatch(path: "extra", message: "unknown field 'extra'"))
  ]
end

test "large payloads with escapes, UTF-8, and >20 keys round-trip cleanly"
  big_hex = "0123456789abcdef".repeat(2048)
  var wide = {}
  for i in 0..24
    wide = wide.set("field_#{i}", String("val_#{i}_\"quoted\"_\n\t\u{7}_é_😀"))
  end
  wide = wide.set("payload", String(big_hex))
  doc = Object(wide)
  encoded = doc.to_text
  assert parse(encoded) == Ok(doc)
  assert parse(doc.pretty) == Ok(doc)
  assert problem_at(
    "{\"a\":1,\"b\":2,\"c\":3,\"d\":4,\"e\":5,\"f\":6,\"g\":7,\"h\":8,\"i\":9,\"j\":10,\"k\":11,\"l\":12,\"m\":13,\"n\":14,\"o\":15,\"p\":16,\"q\":17,\"r\":18,\"c\":19}"
  ) == (1, 119, "DuplicateKey(key: \"c\")")
end
