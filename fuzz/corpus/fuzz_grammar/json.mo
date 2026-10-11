## Reads JSON from standard input and prints it laid out with two-space
## indentation; or, given a path such as items.0.name, prints only the value at
## that path. Numbers are exact decimals, so 0.1 stays 0.1. Not supported:
## numbers with exponents, such as 1e5, and \u escapes in strings.
##
##   json [PATH] < FILE.json
module Json

enum Json
  Null
  Boolean(Bool)
  Number(Decimal)
  String(Text)
  Array(List(Json))
  Object(Map(Text, Json))
end

enum JsonError
  UnexpectedEnd
  Unexpected(at: Int)
  BadNumber(at: Int)
end

fn to_text(problem: JsonError) : Text
  case problem
    UnexpectedEnd: "the JSON ends too soon"
    Unexpected(at: at): "unexpected character at position #{at}"
    BadNumber(at: at): "the number at position #{at} isn't one Mo reads"
  end
end

fn parse(text: Text) : Result(Json, JsonError)
  examples
    parse(" [1, true, null] ") == Ok(Array([Number(1), Boolean(true), Null]))
    parse("{\"a\": {\"b\": \"c\"}}") == Ok(Object({"a": Object({"b": String("c")})}))
    parse("[1,") == Error(UnexpectedEnd)
    parse("[1] 2") == Error(Unexpected(at: 4))
  end

  chars = text.chars
  (value, after) = try value_at(chars, skip_space(chars, 0))
  rest = skip_space(chars, after)
  return Error(Unexpected(at: rest)) if rest < chars.size
  Ok(value)
end

## The value starting at a character that isn't white space, and the position
## after it.
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

fn word_at(chars: List(Char), at: Int, word: Text, value: Json) : Result((Json, Int), JsonError)
  length = word.char_count
  found = chars.drop(at).take(length)
  return Error(UnexpectedEnd) if found.size < length
  return Error(Unexpected(at: at)) if Text.from_chars(found) != word
  Ok((value, at + length))
end

## A string's text, read up to its closing quote. The opening quote is before
## `at`, and `read` holds the characters read so far.
fn string_at(chars: List(Char), at: Int, read: List(Char)) : Result((Text, Int), JsonError)
  c = try chars.get(at).or_fail(UnexpectedEnd)
  case c
    '"': Ok((Text.from_chars(read), at + 1))
    '\\':
      escaped = try chars.get(at + 1).or_fail(UnexpectedEnd)
      plain = try unescape(escaped).or_fail(Unexpected(at: at + 1))
      string_at(chars, at + 2, read.append(plain))
    _: string_at(chars, at + 1, read.append(c))
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

fn number_at(chars: List(Char), at: Int) : Result((Json, Int), JsonError)
  after = number_end(chars, at)
  text = Text.from_chars(chars.drop(at).take(after - at))
  value = try text.to_decimal.or_fail(BadNumber(at: at))
  Ok((Number(value), after))
end

fn number_end(chars: List(Char), at: Int) : Int
  case chars.get(at)
    Some(c) if c == '-' or c == '.' or c.ascii_digit?: number_end(chars, at + 1)
    Some(_) | None: at
  end
end

## The rest of an array, after its opening bracket. `items` holds the values
## read so far.
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

## The rest of an object, after its opening brace. `fields` holds the fields
## read so far.
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

## The position after the expected character.
fn expect(chars: List(Char), at: Int, wanted: Char) : Result(Int, JsonError)
  case chars.get(at)
    Some(c) if c == wanted: Ok(at + 1)
    Some(_): Error(Unexpected(at: at))
    None: Error(UnexpectedEnd)
  end
end

fn skip_space(chars: List(Char), at: Int) : Int
  case chars.get(at)
    Some(c) if c.ascii_white_space?: skip_space(chars, at + 1)
    Some(_) | None: at
  end
end

## The value laid out with two spaces per level, starting at `indent`.
fn pretty(value: Json, indent: Text) : Text
  examples
    pretty(Array([]), "") == "[]"
    pretty(Object({"a": Array([Number(1.5)])}), "") == "{\n  \"a\": [\n    1.5\n  ]\n}"
  end

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

fn quoted(text: Text) : Text
  examples
    quoted("say \"hi\"\n") == "\"say \\\"hi\\\"\\n\""
  end

  escaped = text.replace("\\", "\\\\").replace("\"", "\\\"").replace("\n", "\\n").replace("\t", "\\t")
  "\"#{escaped}\""
end

## The value at a path of object keys and array positions.
fn at_path(value: Json, path: List(Text)) : Option(Json)
  examples
    at_path(Object({"items": Array([Null, Object({"name": String("pen")})])}), ["items", "1", "name"]) == Some(String("pen"))
    at_path(Array([Null]), ["5"]) == None
    at_path(Null, []) == Some(Null)
  end

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

fn read_input(console: Console) : Text
  var lines = []
  while console.read_line is Some(line)
    lines = lines.append(line)
  end
  lines.join("\n")
end

fn main(args: Args, console: Console) : Int
  case parse(read_input(console))
    Error(problem):
      console.error("json: #{problem}")
      1
    Ok(value):
      case args.list
        []:
          console.print(pretty(value, ""))
          0
        [path]:
          case at_path(value, path.split("."))
            Some(found):
              console.print(pretty(found, ""))
              0
            None:
              console.error("json: nothing at #{path}")
              1
          end
        _:
          console.error("usage: json [PATH] < FILE.json")
          2
      end
  end
end
