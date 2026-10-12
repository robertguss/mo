## Lays out JSON for reading. Reads one JSON document from standard input and
## prints it with two spaces of indentation per level; or, given a path such
## as items.0.name, prints only the value at that path, laid out the same way.
## Each part of a path is a key of an object or a position in an array,
## counting from 0.
module Json

## A JSON value. A number holds the text it prints as: its shortest spelling
## when it fits a Decimal, such as 2.5 for 2.50, and otherwise the spelling it
## was written with. An object keeps its keys in the order they first appear,
## and a key given twice keeps its last value.
enum Json
  Null
  Boolean(Bool)
  Number(Text)
  String(Text)
  Array(List(Json))
  Object(Map(Text, Json))
end

## What is wrong with the input, and the position in it where that was found.
struct ParseError
  message: Text
  at: Int
end

## Prints the JSON on standard input laid out, or the value at the path given
## as the one argument. Exits with 1 when the input isn't JSON or has no value
## at the path, and with 2 when given more than one argument.
fn main(args: Args, console: Console) : Int
  path = case args.list
    []: ""
    [given]: given
    _:
      console.error("usage: json [PATH] < FILE, with a PATH such as items.0.name")
      return 2
  end
  case run(read_all(console), path)
    Ok(output):
      console.print(output)
      0
    Error(message):
      console.error("json: #{message}")
      1
  end
end

## All of standard input, its lines joined with line feeds.
fn read_all(console: Console) : Text
  var lines = []
  while console.read_line is Some(line)
    lines = lines.append(line)
  end
  lines.join("\n")
end

## What to print for a document and a path, or why there is nothing to print.
## The empty path stands for the whole document.
fn run(input: Text, path: Text) : Result(Text, Text)
  document = try parse(input)
  value = try find_path(document, path)
  Ok(layout(value, ""))
end

# Reading JSON ---------------------------------------------------------------

## Reads a JSON document: one value, with nothing but white space around it.
## A failure says what is wrong and where, by line and column.
fn parse(input: Text) : Result(Json, Text)
  chars = input.chars
  read_document(chars).map_error(fn(problem)
    "#{position(chars, problem.at)}: #{problem.message}"
  end)
end

## Reads the one value in `chars`, which may have white space around it.
fn read_document(chars: List(Char)) : Result(Json, ParseError)
  (value, after) = try parse_value(chars, skip_space(chars, 0))
  rest = skip_space(chars, after)
  return Error(expecting("the end of the input", chars, rest)) if rest < chars.size
  Ok(value)
end

## Reads the value that starts at `at`, giving it and the position after it.
fn parse_value(chars: List(Char), at: Int) : Result((Json, Int), ParseError)
  case chars.get(at)
    Some('{'): parse_object(chars, at)
    Some('['): parse_array(chars, at)
    Some('"'):
      (text, after) = try parse_string(chars, at)
      Ok((String(text), after))
    Some('t'): parse_word(chars, at, "true", Boolean(true))
    Some('f'): parse_word(chars, at, "false", Boolean(false))
    Some('n'): parse_word(chars, at, "null", Null)
    Some(c) if c == '-' or c.ascii_digit?: parse_number(chars, at)
    None | Some(_): Error(expecting("a value", chars, at))
  end
end

## Reads the word true, false or null, which stands for `value`.
fn parse_word(chars: List(Char), at: Int, word: Text, value: Json) : Result((Json, Int), ParseError)
  var next = at
  for letter in word.chars
    return Error(ParseError(message: "expected #{word}", at: at)) if chars.get(next) != Some(letter)
    next += 1
  end
  Ok((value, next))
end

## Reads a number: an optional minus sign, digits without a leading zero, then
## optionally a fraction and an exponent. A number that fits a Decimal takes
## the Decimal's shortest spelling; any other, such as one with an exponent,
## keeps the spelling it was written with.
fn parse_number(chars: List(Char), at: Int) : Result((Json, Int), ParseError)
  var next = at
  if chars.get(next) == Some('-')
    next += 1
  end
  whole = next
  next = skip_digits(chars, whole)
  return Error(expecting("a digit", chars, whole)) if next == whole
  leading_zero? = next - whole > 1 and chars.get(whole) == Some('0')
  return Error(ParseError(message: "a number can't have a leading zero", at: whole)) if leading_zero?
  if chars.get(next) == Some('.')
    fraction = next + 1
    next = skip_digits(chars, fraction)
    return Error(expecting("a digit after the decimal point", chars, fraction)) if next == fraction
  end
  if chars.get(next) == Some('e') or chars.get(next) == Some('E')
    next += 1
    if chars.get(next) == Some('+') or chars.get(next) == Some('-')
      next += 1
    end
    exponent = next
    next = skip_digits(chars, exponent)
    return Error(expecting("a digit in the exponent", chars, exponent)) if next == exponent
  end
  written = slice(chars, at, next)
  spelling = case written.to_decimal
    Some(n): "#{n}"
    None: written
  end
  Ok((Number(spelling), next))
end

## Reads an array whose "[" is at `open`.
fn parse_array(chars: List(Char), open: Int) : Result((Json, Int), ParseError)
  var next = skip_space(chars, open + 1)
  return Ok((Array([]), next + 1)) if chars.get(next) == Some(']')
  var items = []
  var more? = true
  while more?
    (item, after) = try parse_value(chars, next)
    items = items.append(item)
    next = skip_space(chars, after)
    case chars.get(next)
      Some(','): next = skip_space(chars, next + 1)
      Some(']'): more? = false
      None | Some(_): return Error(expecting("\",\" or \"]\" after an element", chars, next))
    end
  end
  Ok((Array(items), next + 1))
end

## Reads an object whose "{" is at `open`.
fn parse_object(chars: List(Char), open: Int) : Result((Json, Int), ParseError)
  var next = skip_space(chars, open + 1)
  return Ok((Object({}), next + 1)) if chars.get(next) == Some('}')
  var members = {}
  var more? = true
  while more?
    return Error(expecting("a key in double quotes", chars, next)) if chars.get(next) != Some('"')
    (key, after_key) = try parse_string(chars, next)
    colon = skip_space(chars, after_key)
    return Error(expecting("\":\" after the key", chars, colon)) if chars.get(colon) != Some(':')
    (value, after_value) = try parse_value(chars, skip_space(chars, colon + 1))
    members = members.set(key, value)
    next = skip_space(chars, after_value)
    case chars.get(next)
      Some(','): next = skip_space(chars, next + 1)
      Some('}'): more? = false
      None | Some(_): return Error(expecting("\",\" or \"}\" after a member", chars, next))
    end
  end
  Ok((Object(members), next + 1))
end

## Reads a string whose opening quote is at `open`, giving its text, with the
## escapes decoded, and the position after its closing quote.
fn parse_string(chars: List(Char), open: Int) : Result((Text, Int), ParseError)
  var decoded = []
  var next = open + 1
  while chars.get(next) != Some('"')
    case chars.get(next)
      None: return Error(ParseError(message: "a string is never closed", at: open))
      Some('\\'):
        (c, after) = try parse_escape(chars, next)
        decoded = decoded.append(c)
        next = after
      Some(c) if c.code < 32:
        return Error(ParseError(message: "a string must escape control characters, such as line breaks", at: next))
      Some(c):
        decoded = decoded.append(c)
        next += 1
    end
  end
  Ok((Text.from_chars(decoded), next + 1))
end

## Reads the escape whose backslash is at `backslash`, giving the character it
## stands for and the position after it.
fn parse_escape(chars: List(Char), backslash: Int) : Result((Char, Int), ParseError)
  at = backslash + 1
  case chars.get(at)
    Some(c) if c == '"' or c == '\\' or c == '/': Ok((c, at + 1))
    Some('b'): Ok(('\u{8}', at + 1))
    Some('f'): Ok(('\u{C}', at + 1))
    Some('n'): Ok(('\n', at + 1))
    Some('r'): Ok(('\r', at + 1))
    Some('t'): Ok(('\t', at + 1))
    Some('u'): parse_code_point(chars, at + 1)
    None | Some(_): Error(expecting("one of \" \\ / b f n r t u after a backslash", chars, at))
  end
end

## Reads the four hex digits of a \u escape, which start at `at`, together
## with a second \u escape after them when the two make a surrogate pair. It
## gives the character they stand for and the position after the digits.
fn parse_code_point(chars: List(Char), at: Int) : Result((Char, Int), ParseError)
  first = try hex4(chars, at)
  var code = first
  var next = at + 4
  pair? = first >= 0xD800 and first < 0xDC00 and
    chars.get(next) == Some('\\') and chars.get(next + 1) == Some('u')
  if pair?
    second = try hex4(chars, next + 2)
    if second >= 0xDC00 and second < 0xE000
      code = 0x10000 + (first - 0xD800) * 0x400 + (second - 0xDC00)
      next += 6
    end
  end
  half = ParseError(message: "a \\u escape gives only half of a surrogate pair", at: at - 2)
  return Error(half) if code >= 0xD800 and code < 0xE000
  case code.to_char
    Some(c): Ok((c, next))
    None: Error(half)
  end
end

## The number written with the four hex digits that start at `at`.
fn hex4(chars: List(Char), at: Int) : Result(Int, ParseError)
  var code = 0
  for i in at..at + 4
    digit = try hex_digit_at(chars, i).or_fail(expecting("a hex digit", chars, i))
    code = code * 16 + digit
  end
  Ok(code)
end

## The value of the hex digit at `at`, or None when there isn't one there.
fn hex_digit_at(chars: List(Char), at: Int) : Option(Int)
  c = try chars.get(at)
  lower = c.ascii_lowercase
  if c.ascii_digit?
    Some(c.code - '0'.code)
  else if lower >= 'a' and lower <= 'f'
    Some(lower.code - 'a'.code + 10)
  else
    None
  end
end

## The first position at or after `at` that isn't JSON white space: a space,
## tab, line feed or carriage return.
fn skip_space(chars: List(Char), at: Int) : Int
  var next = at
  while chars.get(next) is Some(c) and (c == ' ' or c == '\t' or c == '\n' or c == '\r')
    next += 1
  end
  next
end

## The position after the ASCII digits that start at `at`.
fn skip_digits(chars: List(Char), at: Int) : Int
  var next = at
  while chars.get(next) is Some(c) and c.ascii_digit?
    next += 1
  end
  next
end

## The characters from `from` up to, but not including, `until`, as text.
fn slice(chars: List(Char), from: Int, until: Int) : Text
  Text.from_chars((from..until).to_list.filter_map(fn(i) chars.get(i) end))
end

## A failure to find what was expected at `at`, saying what is there instead.
fn expecting(what: Text, chars: List(Char), at: Int) : ParseError
  found = case chars.get(at)
    Some(c): quote(c.to_text)
    None: "the end of the input"
  end
  ParseError(message: "expected #{what}, found #{found}", at: at)
end

## A position in the input as a line and a column, both counted from 1.
fn position(chars: List(Char), at: Int) : Text
  examples
    position("[1]".chars, 0) == "line 1, column 1"
    position("[\n  x]".chars, 4) == "line 2, column 3"
  end

  var line = 1
  var column = 1
  for c in chars.take(at)
    if c == '\n'
      line += 1
      column = 1
    else
      column += 1
    end
  end
  "line #{line}, column #{column}"
end

# Writing JSON ---------------------------------------------------------------

## The JSON text of a value, laid out with two spaces per level. `indent` is
## the indentation of the line the value starts on.
fn layout(value: Json, indent: Text) : Text
  case value
    Null: "null"
    Boolean(b): if b: "true" else: "false"
    Number(spelling): spelling
    String(text): quote(text)
    Array(items):
      inner = "#{indent}  "
      lines = items.map(fn(item) "#{inner}#{layout(item, inner)}" end)
      enclose("[", lines, "]", indent)
    Object(members):
      inner = "#{indent}  "
      lines = members.entries.map(fn(entry)
        (key, member) = entry
        "#{inner}#{quote(key)}: #{layout(member, inner)}"
      end)
      enclose("{", lines, "}", indent)
  end
end

## Lines between brackets, a comma ending each line but the last, with the
## closing bracket at `indent`; just the two brackets when there are no lines.
fn enclose(open: Text, lines: List(Text), close: Text, indent: Text) : Text
  if lines.empty?
    "#{open}#{close}"
  else
    body = lines.join(",\n")
    "#{open}\n#{body}\n#{indent}#{close}"
  end
end

## A text as a JSON string: in double quotes, with quotes, backslashes and
## control characters escaped.
fn quote(text: Text) : Text
  examples
    quote("Widget") == "\"Widget\""
    quote("say \"hi\"\n") == "\"say \\\"hi\\\"\\n\""
  end

  escaped = text.chars.map(escape).join("")
  "\"#{escaped}\""
end

## How a character is written inside a JSON string.
fn escape(c: Char) : Text
  examples
    escape('a') == "a"
    escape('"') == "\\\""
    escape('\u{1F}') == "\\u001f"
  end

  case c
    '"': "\\\""
    '\\': "\\\\"
    '\n': "\\n"
    '\r': "\\r"
    '\t': "\\t"
    _ if c.code == 8: "\\b"
    _ if c.code == 12: "\\f"
    _ if c.code < 32: "\\u00#{hex_digit(c.code / 16)}#{hex_digit(c.code % 16)}"
    _: c.to_text
  end
end

## The hex digit for a number from 0 to 15.
fn hex_digit(n: Int) : Text
  case "0123456789abcdef".chars.get(n)
    Some(c): c.to_text
    None: stop "hex_digit is given only numbers from 0 to 15"
  end
end

# Paths ----------------------------------------------------------------------

## The value at a path such as items.0.name. Each part of the path is a key of
## an object or a position in an array, counting from 0. Empty parts are
## skipped, so the empty path gives the whole document.
fn find_path(document: Json, path: Text) : Result(Json, Text)
  var current = document
  var walked = []
  for part in path.split(".")
    continue if part.empty?
    walked = walked.append(part)
    current = try step(current, part).or_fail("no value at #{walked.join(".")}")
  end
  Ok(current)
end

## The value inside `value` at one part of a path, if there is one.
fn step(value: Json, part: Text) : Option(Json)
  examples
    step(Object({"a": Null}), "a") == Some(Null)
    step(Array([Null, Boolean(true)]), "1") == Some(Boolean(true))
    step(Array([Null]), "first") == None
    step(String("ab"), "0") == None
  end

  case value
    Object(members): members.get(part)
    Array(items): items.get(try part.to_int)
    Null | Boolean(_) | Number(_) | String(_): None
  end
end

# Tests ----------------------------------------------------------------------

test "a document is laid out with two spaces per level"
  input = "{\"items\": [{\"name\": \"Widget\", \"tags\": []}, 2.50, true], \"note\": null, \"meta\": {}}"
  laid_out = """
    {
      "items": [
        {
          "name": "Widget",
          "tags": []
        },
        2.5,
        true
      ],
      "note": null,
      "meta": {}
    }
    """
  assert run(input, "") == Ok(laid_out)
end

test "a path picks out one value, laid out the same way"
  input = "{\"items\": [{\"name\": \"Widget\"}, {\"name\": \"Gadget\", \"sizes\": [1, 2]}]}"
  assert run(input, "items.1.name") == Ok("\"Gadget\"")
  assert run(input, "items.1.sizes") == Ok("[\n  1,\n  2\n]")
  assert run(input, "items.0") == Ok("{\n  \"name\": \"Widget\"\n}")
  assert run(input, "items.2") == Error("no value at items.2")
  assert run(input, "items.0.name.first") == Error("no value at items.0.name.first")
end

test "strings are decoded, then escaped again only where JSON needs it"
  input = "[\"tab\\there\", \"\\u00e9\\ud83d\\ude00\", \"a\\/b\", \"quote \\\" and \\\\\"]"
  assert run(input, "") == Ok("[\n  \"tab\\there\",\n  \"\u{E9}\u{1F600}\",\n  \"a/b\",\n  \"quote \\\" and \\\\\"\n]")
end

test "a number takes its shortest spelling when it fits a Decimal"
  assert run("[-0.50, 1.0, 10, 1e3, 2E-7]", "") == Ok("[\n  -0.5,\n  1,\n  10,\n  1e3,\n  2E-7\n]")
end

test "a mistake is reported with its line and column"
  assert run("[1, 2,]", "") == Error("line 1, column 7: expected a value, found \"]\"")
  assert run("{\"a\": 1}\n}", "") == Error("line 2, column 1: expected the end of the input, found \"}\"")
  assert run("", "") == Error("line 1, column 1: expected a value, found the end of the input")
  assert run("[007]", "") == Error("line 1, column 2: a number can't have a leading zero")
end

test "main prints the value at the path given on the command line"
  console = Console.fake_input(["{\"items\": [", "  {\"name\": \"Widget\"}", "]}"])
  assert main(Args.of(["items.0.name"]), console) == 0
  assert console.printed == ["\"Widget\""]
end

test "main reports a failure on standard error"
  console = Console.fake_input(["[1, 2]"])
  assert main(Args.of(["5"]), console) == 1
  assert console.printed_errors == ["json: no value at 5"]
end

test "main takes at most one path"
  console = Console.fake_input(["{}"])
  assert main(Args.of(["a", "b"]), console) == 2
  assert console.printed == []
end
