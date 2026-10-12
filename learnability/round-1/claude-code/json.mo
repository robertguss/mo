## Reads JSON from standard input and prints it laid out with two-space
## indentation. Given a path such as items.0.name, it prints only the value at
## that path: each part of the path is the key of an object's member, or the
## position of an array's element, counting from 0.
##
## Numbers are printed as they were written. Strings may use the escapes
## \" \\ \/ \b \f \n \r and \t, but not \u.
module Json

## A JSON value. An object keeps its members in the order they were written.
enum Json
  Null
  Boolean(Bool)
  Number(Text)
  String(Text)
  Array(List(Json))
  Object(List((Text, Json)))
end

## Why a document isn't JSON, and where: lines and columns count from 1.
struct SyntaxError
  message: Text
  line: Int
  column: Int
end

## Why the program can't print what it was asked for.
enum Problem
  BadJson(SyntaxError)
  NoValue(path: Text)
  Usage
end

## How a problem reads on standard error.
fn to_text(problem: Problem) : Text
  case problem
    BadJson(error): "json: line #{error.line}, column #{error.column}: #{error.message}"
    NoValue(path: path): "json: there is no value at #{path}"
    Usage: "usage: json [PATH] < FILE, where PATH is like items.0.name"
  end
end

## Prints the JSON on standard input, or the value at the path given, laid out.
fn main(args: Args, console: Console) : Result(Unit, Problem)
  return Error(Usage) if args.list.size > 1
  path = args.list.first or ""
  document = try parse(read_input(console))
  value = try lookup(document, steps_of(path)).or_fail(NoValue(path: path))
  for line in layout(value)
    console.print(line)
  end
  Ok(Unit)
end

## All of standard input, with its lines joined by line breaks.
fn read_input(console: Console) : Text
  var lines = []
  while console.read_line is Some(line)
    lines = lines.append(line)
  end
  lines.join("\n")
end

# Reading JSON. Each step takes the document's characters and a position in
# them, and gives what it read with the position just past it.

## Reads a document: one value, with nothing but white space around it.
fn parse(source: Text) : Result(Json, SyntaxError)
  examples
    parse(" [1, -2.5e3, true] ") == Ok(Array([Number("1"), Number("-2.5e3"), Boolean(true)]))
    parse("{\"a\": null}") == Ok(Object([("a", Null)]))
  end

  chars = source.chars
  (value, after) = try parse_value(chars, 0)
  rest = skip_space(chars, after)
  return Error(unexpected(chars, rest, "the end of the input")) if rest < chars.size
  Ok(value)
end

## Reads the value at position, after any white space.
fn parse_value(chars: List(Char), position: Int) : Result((Json, Int), SyntaxError)
  start = skip_space(chars, position)
  case chars.get(start)
    Some('{'): parse_object(chars, start + 1)
    Some('['): parse_array(chars, start + 1)
    Some('"'): parse_string(chars, start + 1)
    Some('t'): parse_word(chars, start, "true", Boolean(true))
    Some('f'): parse_word(chars, start, "false", Boolean(false))
    Some('n'): parse_word(chars, start, "null", Null)
    Some(c) if c == '-' or c.ascii_digit?: parse_number(chars, start)
    Some(_) | None: Error(unexpected(chars, start, "a value"))
  end
end

## Reads an object, from just past its opening brace.
fn parse_object(chars: List(Char), position: Int) : Result((Json, Int), SyntaxError)
  start = skip_space(chars, position)
  return Ok((Object([]), start + 1)) if chars.get(start) == Some('}')
  parse_members(chars, start, [])
end

## Reads an object's members from position through its closing brace, after
## the members before them.
fn parse_members(chars: List(Char), position: Int, members: List((Text, Json))) : Result((Json, Int), SyntaxError)
  key_start = skip_space(chars, position)
  return Error(unexpected(chars, key_start, "a key in double quotes")) if chars.get(key_start) != Some('"')
  (key, after_key) = try parse_quoted(chars, key_start + 1)
  colon = skip_space(chars, after_key)
  return Error(unexpected(chars, colon, "':'")) if chars.get(colon) != Some(':')
  (member, after_member) = try parse_value(chars, colon + 1)
  so_far = members.append((key, member))
  next = skip_space(chars, after_member)
  case chars.get(next)
    Some(','): parse_members(chars, next + 1, so_far)
    Some('}'): Ok((Object(so_far), next + 1))
    Some(_) | None: Error(unexpected(chars, next, "',' or '}'"))
  end
end

## Reads an array, from just past its opening bracket.
fn parse_array(chars: List(Char), position: Int) : Result((Json, Int), SyntaxError)
  start = skip_space(chars, position)
  return Ok((Array([]), start + 1)) if chars.get(start) == Some(']')
  parse_elements(chars, start, [])
end

## Reads an array's elements from position through its closing bracket, after
## the elements before them.
fn parse_elements(chars: List(Char), position: Int, items: List(Json)) : Result((Json, Int), SyntaxError)
  (item, after_item) = try parse_value(chars, position)
  so_far = items.append(item)
  next = skip_space(chars, after_item)
  case chars.get(next)
    Some(','): parse_elements(chars, next + 1, so_far)
    Some(']'): Ok((Array(so_far), next + 1))
    Some(_) | None: Error(unexpected(chars, next, "',' or ']'"))
  end
end

## Reads a string, from just past its opening quote.
fn parse_string(chars: List(Char), position: Int) : Result((Json, Int), SyntaxError)
  (text, after) = try parse_quoted(chars, position)
  Ok((String(text), after))
end

## Reads the characters of a string, from just past its opening quote through
## its closing quote, with each escape replaced by the character it stands for.
fn parse_quoted(chars: List(Char), position: Int) : Result((Text, Int), SyntaxError)
  var at = position
  var letters = []
  while chars.get(at) is Some(c) and c != '"'
    if c == '\\'
      letter = try unescape(chars, at + 1)
      letters = letters.append(letter)
      at += 2
    else if c.code < 32
      return Error(syntax_error(chars, at, "a line break or other control character in a string must be escaped"))
    else
      letters = letters.append(c)
      at += 1
    end
  end
  return Error(syntax_error(chars, position - 1, "this string has no closing quote")) if chars.get(at) is None
  Ok((Text.from_chars(letters), at + 1))
end

## The character that an escape stands for, from just past its backslash.
fn unescape(chars: List(Char), position: Int) : Result(Char, SyntaxError)
  case chars.get(position)
    Some('"'): Ok('"')
    Some('\\'): Ok('\\')
    Some('/'): Ok('/')
    Some('b'): Ok('\u{8}')
    Some('f'): Ok('\u{C}')
    Some('n'): Ok('\n')
    Some('r'): Ok('\r')
    Some('t'): Ok('\t')
    Some('u'): Error(syntax_error(chars, position - 1, "\\u escapes are not supported"))
    Some(c): Error(syntax_error(chars, position - 1, "\\#{c} is not an escape"))
    None: Error(syntax_error(chars, position, "this string has no closing quote"))
  end
end

## Reads a number, keeping its spelling: an optional minus sign, a whole part
## with no leading zeros, then an optional fraction and an optional exponent.
fn parse_number(chars: List(Char), position: Int) : Result((Json, Int), SyntaxError)
  var at = position
  if chars.get(at) == Some('-')
    at += 1
  end
  whole_end = skip_digits(chars, at)
  return Error(unexpected(chars, at, "a digit")) if whole_end == at
  return Error(syntax_error(chars, at, "a number can't start with 0 and another digit")) if chars.get(at) == Some('0') and whole_end > at + 1
  at = whole_end
  if chars.get(at) == Some('.')
    fraction_end = skip_digits(chars, at + 1)
    return Error(unexpected(chars, at + 1, "a digit after the decimal point")) if fraction_end == at + 1
    at = fraction_end
  end
  if chars.get(at) == Some('e') or chars.get(at) == Some('E')
    var digits_start = at + 1
    if chars.get(digits_start) == Some('+') or chars.get(digits_start) == Some('-')
      digits_start += 1
    end
    exponent_end = skip_digits(chars, digits_start)
    return Error(unexpected(chars, digits_start, "a digit in the exponent")) if exponent_end == digits_start
    at = exponent_end
  end
  Ok((Number(text_between(chars, position, at)), at))
end

## Reads the word, such as true, that stands for value.
fn parse_word(chars: List(Char), position: Int, word: Text, value: Json) : Result((Json, Int), SyntaxError)
  var at = position
  for letter in word.chars
    return Error(syntax_error(chars, position, "expected #{word}")) if chars.get(at) != Some(letter)
    at += 1
  end
  Ok((value, at))
end

## The characters from position from up to, but not including, position to.
fn text_between(chars: List(Char), from: Int, to: Int) : Text
  Text.from_chars((from..to).to_list.filter_map(fn(i) chars.get(i) end))
end

## The first position at or after position that isn't white space.
fn skip_space(chars: List(Char), position: Int) : Int
  var at = position
  while chars.get(at) is Some(c) and space?(c)
    at += 1
  end
  at
end

## Whether c is white space that may separate the parts of a document.
fn space?(c: Char) : Bool
  c == ' ' or c == '\n' or c == '\r' or c == '\t'
end

## The first position at or after position that isn't an ASCII digit.
fn skip_digits(chars: List(Char), position: Int) : Int
  var at = position
  while chars.get(at) is Some(c) and c.ascii_digit?
    at += 1
  end
  at
end

## A syntax error at position, saying what was expected there and what is there.
fn unexpected(chars: List(Char), position: Int, wanted: Text) : SyntaxError
  syntax_error(chars, position, "expected #{wanted} but found #{shown(chars, position)}")
end

## A syntax error at position, with the line and column it is on.
fn syntax_error(chars: List(Char), position: Int, message: Text) : SyntaxError
  var line = 1
  var column = 1
  for c in chars.take(position)
    if c == '\n'
      line += 1
      column = 1
    else
      column += 1
    end
  end
  SyntaxError(message: message, line: line, column: column)
end

## The character at position, as a message shows it.
fn shown(chars: List(Char), position: Int) : Text
  case chars.get(position)
    Some(c) if c.code < 32: "control character #{c.code}"
    Some(c): "'#{c}'"
    None: "the end of the input"
  end
end

# Finding the value at a path.

## The steps of a path such as items.0.name. A leading "." is allowed, and an
## empty path leads to the whole document.
fn steps_of(path: Text) : List(Text)
  examples
    steps_of("items.0.name") == ["items", "0", "name"]
    steps_of(".items") == ["items"]
    steps_of("") == []
  end

  trimmed = path.drop_prefix(".") or path
  if trimmed.empty?: [] else: trimmed.split(".")
end

## The value that steps lead to from document, one member or element at a time.
fn lookup(document: Json, steps: List(Text)) : Option(Json)
  var current = document
  for step in steps
    current = try child_at(current, step)
  end
  Some(current)
end

## An object's member with the key, the last if there are several, or an
## array's element at the position the key spells.
fn child_at(value: Json, key: Text) : Option(Json)
  case value
    Object(members): members.filter(fn(member) member.0 == key end).last.map(fn(member) member.1 end)
    Array(items):
      index = try key.to_int
      items.get(index)
    Null | Boolean(_) | Number(_) | String(_): None
  end
end

# Laying values out.

## The lines of value, laid out with two spaces of indentation for each level.
fn layout(value: Json) : List(Text)
  add_layout([], value, "", "", "")
end

## lines, followed by the lines of value. Its first line starts with lead, its
## last line ends with trail, and the members or elements inside it are
## indented two spaces more than indent.
fn add_layout(lines: List(Text), value: Json, indent: Text, lead: Text, trail: Text) : List(Text)
  case value
    Null: lines.append("#{lead}null#{trail}")
    Boolean(flag): lines.append("#{lead}#{flag}#{trail}")
    Number(spelling): lines.append("#{lead}#{spelling}#{trail}")
    String(text): lines.append("#{lead}#{quote(text)}#{trail}")
    Array([]): lines.append("#{lead}[]#{trail}")
    Array(items):
      inner = "#{indent}  "
      var out = lines.append("#{lead}[")
      for (i, item) in items.enumerate
        comma = if i + 1 < items.size: "," else: ""
        out = add_layout(out, item, inner, inner, comma)
      end
      out.append("#{indent}]#{trail}")
    Object([]): lines.append("#{lead}{}#{trail}")
    Object(members):
      inner = "#{indent}  "
      var out = lines.append("#{lead}{")
      for (i, (key, member)) in members.enumerate
        comma = if i + 1 < members.size: "," else: ""
        out = add_layout(out, member, inner, "#{inner}#{quote(key)}: ", comma)
      end
      out.append("#{indent}}#{trail}")
  end
end

## text as a JSON string: in double quotes, with quotes, backslashes and
## control characters escaped.
fn quote(text: Text) : Text
  examples
    quote("pear") == "\"pear\""
    quote("a \"b\"\n") == "\"a \\\"b\\\"\\n\""
  end

  escaped = text.chars.map(escape).join("")
  "\"#{escaped}\""
end

## How c is written inside a JSON string.
fn escape(c: Char) : Text
  case c
    '"': "\\\""
    '\\': "\\\\"
    '\n': "\\n"
    '\r': "\\r"
    '\t': "\\t"
    '\u{8}': "\\b"
    '\u{C}': "\\f"
    _: c.to_text
  end
end

test "a document is laid out with two spaces for each level"
  source = "{\"name\": \"kit\", \"tags\": [\"a\", \"b\"], \"size\": {\"w\": 2.5, \"h\": -1}, \"none\": [], \"ok\": true}"
  assert parse(source) is Ok(document)
  assert layout(document) == [
    "{",
    "  \"name\": \"kit\",",
    "  \"tags\": [",
    "    \"a\",",
    "    \"b\"",
    "  ],",
    "  \"size\": {",
    "    \"w\": 2.5,",
    "    \"h\": -1",
    "  },",
    "  \"none\": [],",
    "  \"ok\": true",
    "}"
  ]
end

test "escapes in strings are read, and written back"
  assert parse("\"a\\\\b\\n\\/\"") is Ok(document)
  assert document == String("a\\b\n/")
  assert layout(document) == ["\"a\\\\b\\n/\""]
end

test "a path prints only the value it leads to"
  console = Console.fake_input(["{\"items\": [{\"name\": \"pear\"},", "{\"name\": \"fig\"}]}"])
  assert main(Args.of(["items.1.name"]), console) == Ok(Unit)
  assert console.printed == ["\"fig\""]
end

test "a path that leads nowhere is a failure"
  console = Console.fake_input(["[1, 2]"])
  assert main(Args.of(["5"]), console) == Error(NoValue(path: "5"))
  assert console.printed == []
end

test "invalid JSON is a failure that says where"
  console = Console.fake_input(["{\"a\": 1,", "  \"b\" 2}"])
  assert main(Args.of([]), console) is Error(BadJson(error))
  assert error.line == 2
  assert error.column == 7
end
