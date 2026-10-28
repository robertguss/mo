module Mcp
expose success, application_error
use Json

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
