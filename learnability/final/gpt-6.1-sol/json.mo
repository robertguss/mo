## Formats JSON from standard input, optionally selecting a dotted path.
module Json

enum JsonValue
  JsonNull
  JsonBool(Bool)
  JsonNumber(Text)
  JsonString(Text)
  JsonArray(List(JsonValue))
  JsonObject(Map(Text, JsonValue))
end

enum JsonError
  InvalidJson(position: Int, reason: Text)
  MissingPath(Text)
  InvalidArguments
end

struct Input
  chars: List(Char)
  position: Int
end

fn to_text(problem: JsonError) : Text
  case problem
    InvalidJson(position: position, reason: reason):
      "JSON character #{position + 1}: #{reason}"
    MissingPath(path): "path not found: #{path}"
    InvalidArguments: "usage: json [items.0.name]"
  end
end

fn skip_space(input: Input) : Input
  var cursor = input
  while cursor.chars.get(cursor.position) is Some(ch)
    break if ch != ' ' and ch != '\t' and ch != '\n' and ch != '\r'
    cursor.position += 1
  end
  cursor
end

fn expect(input: Input, expected: Char) : Result(Input, JsonError)
  if input.chars.get(input.position) == Some(expected)
    Ok(Input(chars: input.chars, position: input.position + 1))
  else
    Error(InvalidJson(position: input.position, reason: "expected '#{expected}'"))
  end
end

fn parse_string(input: Input) : Result((Text, Input), JsonError)
  var cursor = try expect(input, '"')
  var characters = []
  while cursor.chars.get(cursor.position) is Some(ch)
    cursor.position += 1
    return Ok((Text.from_chars(characters), cursor)) if ch == '"'
    if ch == '\\'
      escaped = try cursor.chars.get(cursor.position).or_fail(
        InvalidJson(position: cursor.position, reason: "unfinished string escape")
      )
      cursor.position += 1
      decoded = case escaped
        '"': '"'
        '\\': '\\'
        '/': '/'
        'b': '\u{8}'
        'f': '\u{C}'
        'n': '\n'
        'r': '\r'
        't': '\t'
        'u':
          return Error(InvalidJson(position: cursor.position - 1, reason: "Unicode escapes are not supported"))
        _:
          return Error(InvalidJson(position: cursor.position - 1, reason: "invalid string escape"))
      end
      characters = characters.append(decoded)
    else
      return Error(InvalidJson(position: cursor.position - 1, reason: "unescaped control character")) if ch.code < 32
      characters = characters.append(ch)
    end
  end
  Error(InvalidJson(position: cursor.position, reason: "unterminated string"))
end

# Keep validated number text so large integers and long fractions stay exact.
fn parse_number(input: Input) : Result((JsonValue, Input), JsonError)
  var cursor = input
  if cursor.chars.get(cursor.position) == Some('-')
    cursor.position += 1
  end
  first = try cursor.chars.get(cursor.position).or_fail(
    InvalidJson(position: cursor.position, reason: "expected a digit")
  )
  return Error(InvalidJson(position: cursor.position, reason: "expected a digit")) if not first.ascii_digit?
  if first == '0'
    cursor.position += 1
    if cursor.chars.get(cursor.position) is Some(next) and next.ascii_digit?
      return Error(InvalidJson(position: cursor.position, reason: "leading zero in number"))
    end
  else
    while cursor.chars.get(cursor.position) is Some(digit) and digit.ascii_digit?
      cursor.position += 1
    end
  end
  if cursor.chars.get(cursor.position) == Some('.')
    cursor.position += 1
    fraction_start = cursor.position
    while cursor.chars.get(cursor.position) is Some(digit) and digit.ascii_digit?
      cursor.position += 1
    end
    return Error(InvalidJson(position: cursor.position, reason: "expected a fractional digit")) if cursor.position == fraction_start
  end
  if cursor.chars.get(cursor.position) == Some('e') or cursor.chars.get(cursor.position) == Some('E')
    return Error(InvalidJson(position: cursor.position, reason: "exponents are not supported"))
  end
  spelling = Text.from_chars(input.chars.drop(input.position).take(cursor.position - input.position))
  Ok((JsonNumber(spelling), cursor))
end

fn parse_literal(input: Input, spelling: Text, value: JsonValue) : Result((JsonValue, Input), JsonError)
  var cursor = input
  for ch in spelling.chars
    cursor = try expect(cursor, ch)
  end
  Ok((value, cursor))
end

fn parse_array(input: Input) : Result((JsonValue, Input), JsonError)
  var cursor = skip_space(try expect(input, '['))
  var items = []
  if cursor.chars.get(cursor.position) == Some(']')
    cursor.position += 1
    return Ok((JsonArray(items), cursor))
  end
  while true
    (item, after_item) = try parse_value(cursor)
    items = items.append(item)
    cursor = skip_space(after_item)
    if cursor.chars.get(cursor.position) == Some(']')
      cursor.position += 1
      break
    end
    cursor = skip_space(try expect(cursor, ','))
  end
  Ok((JsonArray(items), cursor))
end

fn parse_object(input: Input) : Result((JsonValue, Input), JsonError)
  var cursor = skip_space(try expect(input, '{'))
  var members = {}
  if cursor.chars.get(cursor.position) == Some('}')
    cursor.position += 1
    return Ok((JsonObject(members), cursor))
  end
  while true
    (key, after_key) = try parse_string(cursor)
    after_colon = try expect(skip_space(after_key), ':')
    (member, after_member) = try parse_value(after_colon)
    # Duplicate keys keep their first position and use their last value.
    members = members.set(key, member)
    cursor = skip_space(after_member)
    if cursor.chars.get(cursor.position) == Some('}')
      cursor.position += 1
      break
    end
    cursor = skip_space(try expect(cursor, ','))
  end
  Ok((JsonObject(members), cursor))
end

fn parse_value(input: Input) : Result((JsonValue, Input), JsonError)
  cursor = skip_space(input)
  ch = try cursor.chars.get(cursor.position).or_fail(
    InvalidJson(position: cursor.position, reason: "expected a JSON value")
  )
  case ch
    '"':
      (text, after_string) = try parse_string(cursor)
      Ok((JsonString(text), after_string))
    '[': parse_array(cursor)
    '{': parse_object(cursor)
    't': parse_literal(cursor, "true", JsonBool(true))
    'f': parse_literal(cursor, "false", JsonBool(false))
    'n': parse_literal(cursor, "null", JsonNull)
    '-': parse_number(cursor)
    digit if digit.ascii_digit?: parse_number(cursor)
    _: Error(InvalidJson(position: cursor.position, reason: "expected a JSON value"))
  end
end

fn quoted(text: Text) : Text
  var pieces = ["\""]
  for ch in text.chars
    piece = case ch
      '"': "\\\""
      '\\': "\\\\"
      '\n': "\\n"
      '\r': "\\r"
      '\t': "\\t"
      '\u{8}': "\\b"
      '\u{C}': "\\f"
      other: other.to_text
    end
    pieces = pieces.append(piece)
  end
  pieces.append("\"").join("")
end

fn pretty(value: JsonValue, indent: Text) : Text
  case value
    JsonNull: "null"
    JsonBool(flag): "#{flag}"
    JsonNumber(spelling): spelling
    JsonString(text): quoted(text)
    JsonArray(items):
      return "[]" if items.empty?
      child_indent = "#{indent}  "
      lines = items.map(fn(item) "#{child_indent}#{pretty(item, child_indent)}" end)
      "[\n#{lines.join(",\n")}\n#{indent}]"
    JsonObject(members):
      return "{}" if members.empty?
      child_indent = "#{indent}  "
      lines = members.entries.map(fn(entry)
        "#{child_indent}#{quoted(entry.0)}: #{pretty(entry.1, child_indent)}"
      end)
      "{\n#{lines.join(",\n")}\n#{indent}}"
  end
end

fn select_path(root: JsonValue, path: Text) : Result(JsonValue, JsonError)
  return Ok(root) if path.empty?
  var current = root
  for part in path.split(".")
    current = case current
      JsonObject(members): try members.get(part).or_fail(MissingPath(path))
      JsonArray(items):
        return Error(MissingPath(path)) if part.empty?
        return Error(MissingPath(path)) if not part.chars.all?(fn(ch) ch.ascii_digit? end)
        index = try part.to_int.or_fail(MissingPath(path))
        try items.get(index).or_fail(MissingPath(path))
      JsonNull | JsonBool(_) | JsonNumber(_) | JsonString(_):
        return Error(MissingPath(path))
    end
  end
  Ok(current)
end

fn main(args: Args, console: Console) : Result(Unit, JsonError)
  return Error(InvalidArguments) if args.list.size > 1
  var lines = []
  while console.read_line is Some(line)
    lines = lines.append(line)
  end
  source = lines.join("\n")
  (root, after_root) = try parse_value(Input(chars: source.chars, position: 0))
  tail = skip_space(after_root)
  if tail.position != tail.chars.size
    return Error(InvalidJson(position: tail.position, reason: "unexpected text after JSON value"))
  end
  selected = try select_path(root, args.list.first or "")
  console.print(pretty(selected, ""))
  Ok(Unit)
end
