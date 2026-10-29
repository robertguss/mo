module Mcp
expose success, application_error, catalog
use Json
use McpSchema
use McpType

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
