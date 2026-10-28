module McpCheck
use Json
use Mcp

fn require(ok: Bool)
  if not ok
    stop "MCP result envelope contract"
  end
end

fn json(text: Text) : Json.Json
  case Json.parse(text)
    Ok(value): value
    Error(_): stop "JSON fixture"
  end
end

fn check_result(result: Json.Json, text: Text, failed: Bool)
  require(
    result ==
      Json.Object({
        "content": Json.Array([
          Json.Object({"type": Json.String("text"), "text": Json.String(text)})
        ]),
        "structuredContent": json(text),
        "isError": Json.Boolean(failed)
      })
  )
end

fn main() : Int
  for (value, success, failure) in [
    (Json.Null, "{\"value\":null}", "{\"error\":{\"kind\":\"application\",\"value\":null}}"),
    (
      Json.String("a\"\\\n\t😀"),
      "{\"value\":\"a\\\"\\\\\\n\\t😀\"}",
      "{\"error\":{\"kind\":\"application\",\"value\":\"a\\\"\\\\\\n\\t😀\"}}"
    ),
    (
      json("{\"case\":\"Error\",\"value\":[false,{},\"-0\"]}"),
      "{\"value\":{\"case\":\"Error\",\"value\":[false,{},\"-0\"]}}",
      "{\"error\":{\"kind\":\"application\",\"value\":{\"case\":\"Error\",\"value\":[false,{},\"-0\"]}}}"
    )
  ]
    check_result(Mcp.success(value), success, false)
    check_result(Mcp.application_error(value), failure, true)
  end
  37
end
