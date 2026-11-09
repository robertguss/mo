module Mcp
expose Catalog(..),
success,
application_error,
tool_result,
tool_reply,
operator_reply,
upgrade_reply,
catalog,
prepare,
page
use Json
use McpRpc
use McpSchema
use McpType
use McpViews

struct Catalog
  tools: List(Json.Json)
  sizes: List(Int)
  image_key: Text
  response_bytes: Int
end

fn catalog(descriptor: McpType.Service) : Json.Json
  var tools = []
  for tool in descriptor.tools
    tools = tools.append(
      Json.Object({
        "name": Json.String(tool.name),
        "description": Json.String(tool.description),
        "inputSchema": McpSchema.input(descriptor.types, tool.arguments),
        "outputSchema": McpSchema.output(descriptor.types, tool.reply, tool.error)
      })
    )
  end
  Json.Object({"tools": Json.Array(tools.concat(McpViews.catalog()))})
end

fn prepare(descriptor: McpType.Service, image_key: Text, response_bytes: Int) : Option(Catalog)
  return None if image_key.byte_count != 16
  for c in image_key.chars
    return None if not ((c >= '0' and c <= '9') or (c >= 'a' and c <= 'f'))
  end
  tools = case catalog(descriptor)
    Json.Object(fields):
      case fields.get("tools")
        Some(Json.Array(items)): items
        None | Some(_): stop "MCP catalog tools"
      end
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): stop "MCP catalog object"
  end
  var sizes = []
  id_bytes = Json.String("\u{0}".repeat(128)).to_text.byte_count
  for i in 0..tools.size
    tool = tools.get(i) or stop "MCP catalog index"
    size = json_bytes(tool)
    next = if i + 1 < tools.size: Some(i + 1) else: None
    if page_size(id_bytes, image_key, next, 1, size) > response_bytes
      return None
    end
    sizes = sizes.append(size)
  end
  Some(Catalog(tools: tools, sizes: sizes, image_key: image_key, response_bytes: response_bytes))
end

# Exact compact JSON length, without allocating a rendered copy of every tool.
fn json_bytes(value: Json.Json) : Int
  examples
    json_bytes(Json.Null) == 4
    json_bytes(Json.Boolean(true)) == 4
    json_bytes(Json.Boolean(false)) == 5
    json_bytes(Json.int(-123)) == 4
    json_bytes(Json.Array([])) == 2
    json_bytes(Json.Object({})) == 2
    json_bytes(Json.String("é😀")) == 8
    json_bytes(Json.String("\"\\\n\r\t\u{8}\u{c}\u{0}\u{1f}")) ==
      Json.String("\"\\\n\r\t\u{8}\u{c}\u{0}\u{1f}").to_text.byte_count
    json_bytes(Json.Object({"é": Json.Array([Json.Null, Json.Boolean(false), Json.int(12)])})) ==
      Json.Object({"é": Json.Array([Json.Null, Json.Boolean(false), Json.int(12)])}).to_text
        .byte_count
  end

  case value
    Json.Null: 4
    Json.Boolean(b): if b: 4 else: 5
    Json.Number(n): n.to_text.byte_count
    Json.String(text): string_bytes(text)
    Json.Array(items):
      var size = 2
      var first = true
      for item in items
        if not first
          size += 1
        end
        first = false
        size += json_bytes(item)
      end
      size
    Json.Object(fields):
      var size = 2
      var first = true
      for (key, item) in fields
        if not first
          size += 1
        end
        first = false
        size += string_bytes(key) + 1 + json_bytes(item)
      end
      size
  end
end

fn string_bytes(text: Text) : Int
  return long_string_bytes(text) if text.byte_count >= 64
  var size = text.byte_count + 2
  for c in text.chars
    case c
      '"' | '\\' | '\n' | '\r' | '\t' | '\u{8}' | '\u{c}': size += 1
      _ if c.code < 32: size += 5
      _: Unit
    end
  end
  size
end

# Native searches avoid a character-list walk for long descriptions.
fn long_string_bytes(text: Text) : Int
  var size = text.byte_count + 2
  for c in "\"\\\n\r\t\u{8}\u{c}".chars
    size += text.byte_count - text.replace(c.to_text, "").byte_count
  end
  for c in "\u{0}\u{1}\u{2}\u{3}\u{4}\u{5}\u{6}\u{7}\u{b}\u{e}\u{f}\u{10}\u{11}\u{12}\u{13}\u{14}\u{15}\u{16}\u{17}\u{18}\u{19}\u{1a}\u{1b}\u{1c}\u{1d}\u{1e}\u{1f}"
    .chars
    size += 5 * (text.byte_count - text.replace(c.to_text, "").byte_count)
  end
  size
end

test "catalog byte sizes match JSON across string lengths and escapes"
  for count in [0, 1, 63, 64, 65, 256]
    for suffix in [
      "",
      "é😀",
      "\"\\",
      "\u{0}\u{1}\u{2}\u{3}\u{4}\u{5}\u{6}\u{7}\u{8}\u{9}\u{a}\u{b}\u{c}\u{d}\u{e}\u{f}\u{10}\u{11}\u{12}\u{13}\u{14}\u{15}\u{16}\u{17}\u{18}\u{19}\u{1a}\u{1b}\u{1c}\u{1d}\u{1e}\u{1f}"
    ]
      value = Json.String("#{"a".repeat(count)}#{suffix.repeat(3)}")
      assert json_bytes(value) == value.to_text.byte_count
    end
  end
end

fn page(catalog: Catalog, start: Int, id: Json.Json) : Option(Json.Json)
  return None if start < 0 or start >= catalog.tools.size
  id_bytes = id.to_text.byte_count
  var items = []
  var bytes = 0
  var after = start
  for i in start..catalog.tools.size
    break if i - start >= 32
    size = catalog.sizes.get(i) or stop "MCP catalog size"
    next = if i + 1 < catalog.tools.size: Some(i + 1) else: None
    if page_size(id_bytes, catalog.image_key, next, i - start + 1, bytes + size) >
      catalog.response_bytes
      break
    end
    items = items.append(catalog.tools.get(i) or stop "MCP catalog index")
    bytes += size
    after = i + 1
  end
  return None if after == start
  next = if after < catalog.tools.size: Some(after) else: None
  Some(McpRpc.result(id, page_result(items, catalog.image_key, next)))
end

fn page_size(id_bytes: Int, key: Text, next: Option(Int), count: Int, bytes: Int) : Int
  base = McpRpc.result(Json.Null, page_result([], key, next)).to_text.byte_count
  base - 4 + id_bytes + bytes + count - 1
end

fn page_result(tools: List(Json.Json), key: Text, next: Option(Int)) : Json.Json
  var fields = {"tools": Json.Array(tools)}
  case next
    Some(index): fields = fields.set("nextCursor", Json.String("#{index}.#{key}"))
    None: Unit
  end
  Json.Object(fields)
end

fn success(value: Json.Json) : Json.Json
  tool_result(tool_content(value, false), false)
end

fn application_error(value: Json.Json) : Json.Json
  tool_result(tool_content(value, true), true)
end

fn tool_content(value: Json.Json, failed: Bool) : Json.Json
  if failed
    Json.Object({"error": Json.Object({"kind": Json.String("application"), "value": value})})
  else
    Json.Object({"value": value})
  end
end

fn tool_reply(id: Json.Json, value: Json.Json, failed: Bool, byte_limit: Int) : Option(Text)
  bounded_reply(id, tool_content(value, failed), failed, byte_limit)
end

## Operator refusals remain operator payloads, not author application errors.
fn operator_reply(id: Json.Json, value: Json.Json, refused: Bool, byte_limit: Int) : Option(Text)
  bounded_reply(id, tool_content(value, false), refused, byte_limit)
end

## An upgrade tool's result: the shared report as structured content and its text rendering.
fn upgrade_reply(id: Json.Json, report: Text, text: Text, refused: Bool, byte_limit: Int) : Option(
  Text
)
  content = case Json.parse(report)
    Ok(value): value
    Error(_): return None
  end
  return None if report.byte_count + text.byte_count > byte_limit / 2
  body = McpRpc.result(id, rendered_tool_result(content, text, refused)).to_text
  if body.byte_count > byte_limit: None else: Some(body)
end

fn bounded_reply(id: Json.Json, content: Json.Json, failed: Bool, byte_limit: Int) : Option(Text)
  text = content.to_text
  return None if text.byte_count > byte_limit / 2
  body = McpRpc.result(id, rendered_tool_result(content, text, failed)).to_text
  if body.byte_count > byte_limit: None else: Some(body)
end

fn tool_result(content: Json.Json, failed: Bool) : Json.Json
  rendered_tool_result(content, content.to_text, failed)
end

fn rendered_tool_result(content: Json.Json, text: Text, failed: Bool) : Json.Json
  Json.Object({
    "content": Json.Array([Json.Object({"type": Json.String("text"), "text": Json.String(text)})]),
    "structuredContent": content,
    "isError": Json.Boolean(failed)
  })
end
