module Mcp
expose Catalog(..), success, application_error, tool_result, catalog, prepare, page
use Json
use McpRpc
use McpSchema
use McpType

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
  Json.Object({"tools": Json.Array(tools)})
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
    size = tool.to_text.byte_count
    next = if i + 1 < tools.size: Some(i + 1) else: None
    if page_size(id_bytes, image_key, next, 1, size) > response_bytes
      return None
    end
    sizes = sizes.append(size)
  end
  if tools.empty? and
    McpRpc.result(Json.String("\u{0}".repeat(128)), page_result([], image_key, None)).to_text
      .byte_count >
      response_bytes
    return None
  end
  Some(Catalog(tools: tools, sizes: sizes, image_key: image_key, response_bytes: response_bytes))
end

fn page(catalog: Catalog, start: Int, id: Json.Json) : Option(Json.Json)
  if start == 0 and catalog.tools.empty?
    value = McpRpc.result(id, page_result([], catalog.image_key, None))
    return if value.to_text.byte_count <= catalog.response_bytes: Some(value) else: None
  end
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
  tool_result(Json.Object({"value": value}), false)
end

fn application_error(value: Json.Json) : Json.Json
  tool_result(
    Json.Object({"error": Json.Object({"kind": Json.String("application"), "value": value})}),
    true
  )
end

fn tool_result(content: Json.Json, failed: Bool) : Json.Json
  Json.Object({
    "content": Json.Array([
      Json.Object({"type": Json.String("text"), "text": Json.String(content.to_text)})
    ]),
    "structuredContent": content,
    "isError": Json.Boolean(failed)
  })
end
