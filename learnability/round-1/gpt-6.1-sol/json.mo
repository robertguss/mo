## Parses and formats JSON, or selects a dot-separated object/array path.
## Exponents and Unicode escape sequences are deliberately unsupported.
module Json

enum Value
  NullValue
  BooleanValue(Bool)
  NumberValue(Decimal)
  StringValue(Text)
  ArrayValue(List(Value))
  ObjectValue(Map(Text, Value))
end

struct Cursor
  chars: List(Char)
  position: Int
end

enum JsonError
  Syntax(position: Int, reason: Text)
  MissingPath(segment: Text)
end

fn whitespace(cursor: Cursor) : Cursor
  var next = cursor
  while next.chars.get(next.position) is Some(ch) and
    (ch == ' ' or ch == '\t' or ch == '\r' or ch == '\n')
    next.position += 1
  end
  next
end

fn read_string(cursor: Cursor) : Result((Text, Cursor), JsonError)
  var next = cursor
  return Error(Syntax(position: next.position + 1, reason: "expected a string")) if next.chars.get(next.position) != Some('"')
  next.position += 1
  var text = ""
  while next.chars.get(next.position) is Some(ch)
    next.position += 1
    return Ok((text, next)) if ch == '"'
    return Error(Syntax(position: next.position, reason: "unescaped control character")) if ch.code < 32
    if ch == '\\'
      escaped = try next.chars.get(next.position).or_fail(
        Syntax(position: next.position + 1, reason: "unfinished escape"))
      next.position += 1
      decoded = case escaped
        '"': '"'
        '\\': '\\'
        '/': '/'
        'b': '\u{8}'
        'f': '\u{C}'
        'n': '\n'
        'r': '\r'
        't': '\t'
        _: return Error(Syntax(position: next.position, reason: "unsupported escape"))
      end
      text = "#{text}#{decoded}"
    else
      text = "#{text}#{ch}"
    end
  end
  Error(Syntax(position: next.position + 1, reason: "unclosed string"))
end

fn literal(cursor: Cursor, spelling: Text, value: Value) : Result((Value, Cursor), JsonError)
  remaining = Text.from_chars(cursor.chars.drop(cursor.position).take(spelling.char_count))
  return Error(Syntax(position: cursor.position + 1, reason: "invalid literal")) if remaining != spelling
  Ok((value, Cursor(chars: cursor.chars, position: cursor.position + spelling.char_count)))
end

fn number(cursor: Cursor) : Result((Value, Cursor), JsonError)
  var next = cursor
  if next.chars.get(next.position) == Some('-')
    next.position += 1
  end
  first = try next.chars.get(next.position).or_fail(
    Syntax(position: next.position + 1, reason: "expected a digit"))
  return Error(Syntax(position: next.position + 1, reason: "expected a digit")) if not first.ascii_digit?
  if first == '0'
    next.position += 1
    if next.chars.get(next.position) is Some(ch) and ch.ascii_digit?
      return Error(Syntax(position: next.position + 1, reason: "leading zero"))
    end
  else
    while next.chars.get(next.position) is Some(ch) and ch.ascii_digit?
      next.position += 1
    end
  end
  if next.chars.get(next.position) == Some('.')
    next.position += 1
    start = next.position
    while next.chars.get(next.position) is Some(ch) and ch.ascii_digit?
      next.position += 1
    end
    return Error(Syntax(position: next.position + 1, reason: "expected fractional digits")) if start == next.position
  end
  spelling = Text.from_chars(next.chars.drop(cursor.position).take(next.position - cursor.position))
  parsed = try spelling.to_decimal.or_fail(
    Syntax(position: cursor.position + 1, reason: "number outside Decimal precision or range"))
  Ok((NumberValue(parsed), next))
end

fn array(cursor: Cursor) : Result((Value, Cursor), JsonError)
  var next = whitespace(Cursor(chars: cursor.chars, position: cursor.position + 1))
  var values = []
  if next.chars.get(next.position) == Some(']')
    next.position += 1
    return Ok((ArrayValue(values), next))
  end
  while true
    (value, after_value) = try parse_value(next)
    values = values.append(value)
    next = whitespace(after_value)
    case next.chars.get(next.position)
      Some(']'):
        next.position += 1
        return Ok((ArrayValue(values), next))
      Some(','): next.position += 1
      Some(_) | None:
        return Error(Syntax(position: next.position + 1, reason: "expected comma or ]"))
    end
    next = whitespace(next)
  end
  stop "the array loop only exits by returning"
end

fn object(cursor: Cursor) : Result((Value, Cursor), JsonError)
  var next = whitespace(Cursor(chars: cursor.chars, position: cursor.position + 1))
  var values = {}
  if next.chars.get(next.position) == Some('}')
    next.position += 1
    return Ok((ObjectValue(values), next))
  end
  while true
    (key, after_key) = try read_string(next)
    next = whitespace(after_key)
    return Error(Syntax(position: next.position + 1, reason: "expected colon")) if next.chars.get(next.position) != Some(':')
    next.position += 1
    (value, after_value) = try parse_value(next)
    values = values.set(key, value)
    next = whitespace(after_value)
    case next.chars.get(next.position)
      Some('}'):
        next.position += 1
        return Ok((ObjectValue(values), next))
      Some(','): next.position += 1
      Some(_) | None:
        return Error(Syntax(position: next.position + 1, reason: "expected comma or }"))
    end
    next = whitespace(next)
  end
  stop "the object loop only exits by returning"
end

fn parse_value(cursor: Cursor) : Result((Value, Cursor), JsonError)
  next = whitespace(cursor)
  case next.chars.get(next.position)
    Some('"'):
      (text, after_string) = try read_string(next)
      Ok((StringValue(text), after_string))
    Some('['): array(next)
    Some('{'): object(next)
    Some('t'): literal(next, "true", BooleanValue(true))
    Some('f'): literal(next, "false", BooleanValue(false))
    Some('n'): literal(next, "null", NullValue)
    Some(ch) if ch == '-' or ch.ascii_digit?: number(next)
    Some(_) | None: Error(Syntax(position: next.position + 1, reason: "expected a JSON value"))
  end
end

fn parse(text: Text) : Result(Value, JsonError)
  (value, cursor) = try parse_value(Cursor(chars: text.chars, position: 0))
  next = whitespace(cursor)
  return Error(Syntax(position: next.position + 1, reason: "trailing input")) if next.position != next.chars.size
  Ok(value)
end

fn quote(text: Text) : Text
  var escaped = "\""
  hex = "0123456789abcdef".chars
  for ch in text.chars
    piece = case ch
      '"': "\\\""
      '\\': "\\\\"
      '\n': "\\n"
      '\r': "\\r"
      '\t': "\\t"
      '\u{8}': "\\b"
      '\u{C}': "\\f"
      other:
        if other.code < 32
          high = hex.get(other.code / 16) or '0'
          low = hex.get(other.code % 16) or '0'
          "\\u00#{high}#{low}"
        else
          other.to_text
        end
    end
    escaped = "#{escaped}#{piece}"
  end
  "#{escaped}\""
end

fn pretty(value: Value, depth: Int) : Text
  indent = "  ".repeat(depth)
  child_indent = "  ".repeat(depth + 1)
  case value
    NullValue: "null"
    BooleanValue(flag?): if flag?: "true" else: "false"
    NumberValue(n): n.to_text
    StringValue(text): quote(text)
    ArrayValue(items):
      if items.empty?
        "[]"
      else
        rows = items.map(fn(item) "#{child_indent}#{pretty(item, depth + 1)}" end)
        "[\n#{rows.join(",\n")}\n#{indent}]"
      end
    ObjectValue(entries):
      if entries.empty?
        "{}"
      else
        rows = entries.entries.map(fn(entry)
          "#{child_indent}#{quote(entry.0)}: #{pretty(entry.1, depth + 1)}"
        end)
        "{\n#{rows.join(",\n")}\n#{indent}}"
      end
  end
end

fn select(value: Value, path: Text) : Result(Value, JsonError)
  return Ok(value) if path.empty?
  var current = value
  for segment in path.split(".")
    current = case current
      ObjectValue(entries): try entries.get(segment).or_fail(MissingPath(segment: segment))
      ArrayValue(items):
        return Error(MissingPath(segment: segment)) if segment.empty? or
          not segment.chars.all?(fn(ch) ch.ascii_digit? end)
        index = try segment.to_int.or_fail(MissingPath(segment: segment))
        try items.get(index).or_fail(MissingPath(segment: segment))
      NullValue | BooleanValue(_) | NumberValue(_) | StringValue(_):
        return Error(MissingPath(segment: segment))
    end
  end
  Ok(current)
end

fn main(args: Args, console: Console) : Int
  if args.list.size > 1
    console.error("Usage: json [DOT.PATH]")
    return 1
  end
  var lines = []
  while console.read_line is Some(line)
    lines = lines.append(line)
  end
  parsed = case parse(lines.join("\n"))
    Ok(value): value
    Error(problem):
      console.error("#{problem}")
      return 1
  end
  chosen = case select(parsed, args.list.first or "")
    Ok(value): value
    Error(problem):
      console.error("#{problem}")
      return 1
  end
  console.print(pretty(chosen, 0))
  0
end
